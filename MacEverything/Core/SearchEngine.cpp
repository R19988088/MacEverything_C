#include "SearchEngine.h"
#include "IndexWAL.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dispatch/dispatch.h>

std::string SearchEngine::toLower(const std::string& s) {
    std::string result;
    result.resize(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return result;
}

std::string SearchEngine::makeFullPath(const std::string& path, const std::string& name) {
    if (path.empty() || path.back() == '/') return path + name;
    return path + "/" + name;
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    std::unique_lock lock(mutex_);

    records_ = std::move(records);
    lowerNames_.resize(records_.size());
    lowerPaths_.resize(records_.size());

    // Parallelize lowercase pre-computation
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;

    size_t chunkSize = (records_.size() + numThreads - 1) / numThreads;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (unsigned t = 0; t < numThreads; t++) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, records_.size());
        if (start >= end) break;

        threads.emplace_back([this, start, end] {
            for (size_t i = start; i < end; i++) {
                lowerNames_[i] = toLower(records_[i].name);
                lowerPaths_[i] = toLower(makeFullPath(records_[i].path, records_[i].name));
            }
        });
    }

    for (auto& th : threads) th.join();

    // Build path index (case-insensitive keys for macOS APFS compatibility)
    pathIndex_.clear();
    pathIndex_.reserve(records_.size());
    for (size_t i = 0; i < records_.size(); i++) {
        pathIndex_[toLower(makeFullPath(records_[i].path, records_[i].name))] = static_cast<uint32_t>(i);
    }

    liveCount_.store(static_cast<uint32_t>(records_.size()), std::memory_order_relaxed);
}

bool SearchEngine::isGlobPattern(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

bool SearchEngine::globMatch(const std::string& pattern, const std::string& text) {
    size_t px = 0, tx = 0;
    size_t starPx = std::string::npos, starTx = 0;

    while (tx < text.size()) {
        if (px < pattern.size() && (pattern[px] == '?' || pattern[px] == text[tx])) {
            px++;
            tx++;
        } else if (px < pattern.size() && pattern[px] == '*') {
            starPx = px++;
            starTx = tx;
        } else if (starPx != std::string::npos) {
            px = starPx + 1;
            tx = ++starTx;
        } else {
            return false;
        }
    }

    while (px < pattern.size() && pattern[px] == '*') px++;
    return px == pattern.size();
}

std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults) const {
    if (keyword.empty()) return {};

    std::shared_lock lock(mutex_);

    if (records_.empty()) return {};

    std::string lowerKey = toLower(keyword);
    bool useGlob = isGlobPattern(lowerKey);

    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;

    size_t totalSize = records_.size();
    size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

    // Per-thread result buffers: pair<index, priority>
    // Priority: 0=name exact match, 1=name starts with, 2=name contains, 3=path-only match
    std::vector<std::vector<std::pair<uint32_t, uint8_t>>> threadResults(numThreads);

    auto* threadResultsPtr = &threadResults;
    const auto* recordsPtr = &records_;
    const auto* lowerNamesPtr = &lowerNames_;
    const auto* lowerPathsPtr = &lowerPaths_;

    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_apply(numThreads, queue, ^(size_t t) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, totalSize);
        if (start >= end) return;

        auto& local = (*threadResultsPtr)[t];
        for (size_t i = start; i < end; i++) {
            // Skip tombstoned records
            if ((*recordsPtr)[i].type == 0) continue;

            const auto& lowerName = (*lowerNamesPtr)[i];
            const auto& lowerPath = (*lowerPathsPtr)[i];

            bool nameMatch = useGlob ? globMatch(lowerKey, lowerName)
                                     : (lowerName.find(lowerKey) != std::string::npos);
            bool pathMatch = !nameMatch && (useGlob ? globMatch(lowerKey, lowerPath)
                                                    : (lowerPath.find(lowerKey) != std::string::npos));

            if (nameMatch || pathMatch) {
                uint8_t priority;
                if (nameMatch) {
                    if (lowerName == lowerKey) {
                        priority = 0; // exact match
                    } else if (lowerName.size() >= lowerKey.size() &&
                               lowerName.compare(0, lowerKey.size(), lowerKey) == 0) {
                        priority = 1; // starts with
                    } else {
                        priority = 2; // contains
                    }
                } else {
                    priority = 3; // path-only match
                }

                local.emplace_back(static_cast<uint32_t>(i), priority);
            }
        }
    });

    // Merge all thread results
    size_t total = 0;
    for (auto& v : threadResults) total += v.size();

    std::vector<std::pair<uint32_t, uint8_t>> merged;
    merged.reserve(total);
    for (auto& v : threadResults) {
        merged.insert(merged.end(), v.begin(), v.end());
    }

    // Sort by priority, then by path length (shorter = shallower = better)
    std::sort(merged.begin(), merged.end(),
        [this](const std::pair<uint32_t, uint8_t>& a, const std::pair<uint32_t, uint8_t>& b) {
            if (a.second != b.second) return a.second < b.second;
            return lowerPaths_[a.first].size() < lowerPaths_[b.first].size();
        });

    // Extract indices, truncated to maxResults
    size_t resultCount = merged.size();
    if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;

    std::vector<uint32_t> result;
    result.reserve(resultCount);
    for (size_t i = 0; i < resultCount; i++) {
        result.push_back(merged[i].first);
    }

    return result;
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= records_.size()) return {};
    return records_[index];
}

uint32_t SearchEngine::recordCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(records_.size());
}

uint32_t SearchEngine::addRecord(FileRecord&& record) {
    std::unique_lock lock(mutex_);

    uint32_t idx = static_cast<uint32_t>(records_.size());
    std::string fullPath = makeFullPath(record.path, record.name);
    std::string lower = toLower(record.name);
    std::string lowerPath = toLower(fullPath);

    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    records_.push_back(std::move(record));
    lowerNames_.push_back(std::move(lower));
    lowerPaths_.push_back(std::move(lowerPath));
    pathIndex_[toLower(fullPath)] = idx;
    liveCount_.fetch_add(1, std::memory_order_relaxed);

    return idx;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    std::unique_lock lock(mutex_);

    auto it = pathIndex_.find(toLower(fullPath));
    if (it == pathIndex_.end()) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    uint32_t idx = it->second;
    records_[idx].type = 0;
    records_[idx].name.clear();
    records_[idx].path.clear();
    records_[idx].size = 0;
    records_[idx].modTime = 0;
    lowerNames_[idx].clear();
    lowerPaths_[idx].clear();
    pathIndex_.erase(it);
    liveCount_.fetch_sub(1, std::memory_order_relaxed);

    return true;
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    std::unique_lock lock(mutex_);

    std::string lowerPrefix = toLower(pathPrefix);
    uint32_t removed = 0;
    // Collect paths to remove (can't erase from pathIndex_ while iterating)
    std::vector<std::string> toRemove;
    for (const auto& [path, idx] : pathIndex_) {
        // pathIndex_ keys are already lowercase
        if (path.size() >= lowerPrefix.size() &&
            path.compare(0, lowerPrefix.size(), lowerPrefix) == 0 &&
            (path.size() == lowerPrefix.size() || path[lowerPrefix.size()] == '/')) {
            toRemove.push_back(path);
        }
    }

    for (const auto& path : toRemove) {
        auto it = pathIndex_.find(path);
        if (it == pathIndex_.end()) continue;
        uint32_t idx = it->second;
        records_[idx].type = 0;
        records_[idx].name.clear();
        records_[idx].path.clear();
        records_[idx].size = 0;
        records_[idx].modTime = 0;
        lowerNames_[idx].clear();
        lowerPaths_[idx].clear();
        pathIndex_.erase(it);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removed++;
    }

    return removed;
}


void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    std::unique_lock lock(mutex_);

    if (wal_) wal_->append(WALOp::Update, fullPath, updated);

    // Remove old record if exists (case-insensitive lookup)
    auto it = pathIndex_.find(toLower(fullPath));
    if (it != pathIndex_.end()) {
        uint32_t idx = it->second;
        records_[idx].type = 0;
        records_[idx].name.clear();
        records_[idx].path.clear();
        records_[idx].size = 0;
        records_[idx].modTime = 0;
        lowerNames_[idx].clear();
        lowerPaths_[idx].clear();
        pathIndex_.erase(it);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(records_.size());
    std::string newFullPath = makeFullPath(updated.path, updated.name);
    std::string lower = toLower(updated.name);
    std::string lowerPath = toLower(newFullPath);

    records_.push_back(std::move(updated));
    lowerNames_.push_back(std::move(lower));
    lowerPaths_.push_back(std::move(lowerPath));
    pathIndex_[lowerPath] = newIdx;
    liveCount_.fetch_add(1, std::memory_order_relaxed);
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    std::unique_lock lock(mutex_);

    uint32_t live = liveCount_.load(std::memory_order_relaxed);
    if (live == records_.size()) return {}; // nothing to compact

    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(live);

    std::vector<FileRecord> newRecords;
    newRecords.reserve(live);
    std::vector<std::string> newLowerNames;
    newLowerNames.reserve(live);
    std::vector<std::string> newLowerPaths;
    newLowerPaths.reserve(live);
    std::unordered_map<std::string, uint32_t> newPathIndex;
    newPathIndex.reserve(live);

    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue;
        uint32_t newIdx = static_cast<uint32_t>(newRecords.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        std::string fullPath = toLower(makeFullPath(records_[i].path, records_[i].name));
        newPathIndex[fullPath] = newIdx;
        newLowerNames.push_back(std::move(lowerNames_[i]));
        newLowerPaths.push_back(std::move(lowerPaths_[i]));
        newRecords.push_back(std::move(records_[i]));
    }

    records_ = std::move(newRecords);
    lowerNames_ = std::move(newLowerNames);
    lowerPaths_ = std::move(newLowerPaths);
    pathIndex_ = std::move(newPathIndex);

    return remap;
}

void SearchEngine::attachWAL(std::shared_ptr<IndexWAL> wal) {
    std::unique_lock lock(mutex_);
    wal_ = std::move(wal);
}

void SearchEngine::detachWAL() {
    std::unique_lock lock(mutex_);
    wal_.reset();
}

// --- Persistence ---

static constexpr char MAGIC[4] = {'M', 'E', 'I', 'D'};
static constexpr uint32_t FORMAT_VERSION_V1 = 1;
static constexpr uint32_t FORMAT_VERSION_V2 = 2;
static constexpr uint32_t FORMAT_VERSION_V3 = 3;

// --- Helper: write/read a length-prefixed string ---
static bool writeString(FILE* f, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    if (fwrite(&len, sizeof(uint32_t), 1, f) != 1) return false;
    if (len > 0 && fwrite(s.data(), 1, len, f) != len) return false;
    return true;
}

static bool readString(FILE* f, std::string& s) {
    uint32_t len;
    if (fread(&len, sizeof(uint32_t), 1, f) != 1) return false;
    if (len > 65536) return false; // sanity limit matching WAL reader
    s.resize(len);
    if (len > 0 && fread(s.data(), 1, len, f) != len) return false;
    return true;
}

// --- Write a single record (v2/v3 format with inode+devId) ---
static bool writeRecord(FILE* f, const FileRecord& r) {
    if (!writeString(f, r.name)) return false;
    if (!writeString(f, r.path)) return false;
    if (fwrite(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fwrite(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod = static_cast<int64_t>(r.modTime);
    if (fwrite(&mod, sizeof(int64_t), 1, f) != 1) return false;
    if (fwrite(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
    if (fwrite(&r.devId, sizeof(int32_t), 1, f) != 1) return false;
    return true;
}

// --- Read a single record, version-aware ---
static bool readRecordFromFile(FILE* f, FileRecord& r, uint32_t version) {
    if (!readString(f, r.name)) return false;
    if (!readString(f, r.path)) return false;
    if (fread(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fread(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod;
    if (fread(&mod, sizeof(int64_t), 1, f) != 1) return false;
    r.modTime = static_cast<time_t>(mod);
    if (version >= FORMAT_VERSION_V2) {
        if (fread(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
        if (fread(&r.devId, sizeof(int32_t), 1, f) != 1) return false;
    }
    return true;
}

// --- v3 save: extensible metadata ---
bool SearchEngine::saveToFile(const std::string& filePath, const IndexMetadata& metadata) const {
    std::shared_lock lock(mutex_);

    std::string tmpPath = filePath + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return false;

    // Helper to check fwrite return values
    bool writeOk = true;
    auto safeWrite = [&](const void* ptr, size_t size, size_t count) {
        if (writeOk && fwrite(ptr, size, count, f) != count) writeOk = false;
    };

    // Header: MAGIC(4) + version(4) + timestamp(8) + lastEventId(8)
    safeWrite(MAGIC, 1, 4);
    uint32_t version = FORMAT_VERSION_V3;
    safeWrite(&version, sizeof(uint32_t), 1);
    int64_t ts = metadata.timestamp > 0 ? metadata.timestamp : static_cast<int64_t>(time(nullptr));
    safeWrite(&ts, sizeof(int64_t), 1);
    safeWrite(&metadata.lastEventId, sizeof(uint64_t), 1);

    // Metadata section: metadataCount(4) + [keyLen(4) + key + valueLen(4) + value] ...
    uint32_t metaCount = static_cast<uint32_t>(metadata.extra.size());
    safeWrite(&metaCount, sizeof(uint32_t), 1);
    for (const auto& [key, value] : metadata.extra) {
        if (!writeOk) break;
        if (!writeString(f, key) || !writeString(f, value)) writeOk = false;
    }

    // Record count — count actual live records to ensure consistency
    uint32_t liveCount = 0;
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type != 0) liveCount++;
    }
    safeWrite(&liveCount, sizeof(uint32_t), 1);

    if (!writeOk) {
        fclose(f);
        remove(tmpPath.c_str());
        return false;
    }

    // Records (same binary layout as v2)
    for (size_t i = 0; i < records_.size(); i++) {
        const auto& r = records_[i];
        if (r.type == 0) continue;
        if (!writeRecord(f, r)) {
            fclose(f);
            remove(tmpPath.c_str());
            return false;
        }
    }

    fclose(f);

    if (rename(tmpPath.c_str(), filePath.c_str()) != 0) {
        remove(tmpPath.c_str());
        return false;
    }
    return true;
}

// --- Legacy save overload ---
bool SearchEngine::saveToFile(const std::string& filePath, uint64_t lastEventId) const {
    IndexMetadata meta;
    meta.lastEventId = lastEventId;
    return saveToFile(filePath, meta);
}

// --- Load: supports v1, v2, and v3 ---
bool SearchEngine::loadFromFile(const std::string& filePath, IndexMetadata* outMetadata) {
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return false;

    // Read & verify magic
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }

    uint32_t version;
    if (fread(&version, sizeof(uint32_t), 1, f) != 1) { fclose(f); return false; }
    if (version < FORMAT_VERSION_V1 || version > FORMAT_VERSION_V3) {
        fclose(f);
        return false;
    }

    IndexMetadata meta;
    meta.formatVersion = version;

    // Timestamp (all versions)
    if (fread(&meta.timestamp, sizeof(int64_t), 1, f) != 1) { fclose(f); return false; }

    // lastEventId (v2+)
    if (version >= FORMAT_VERSION_V2) {
        if (fread(&meta.lastEventId, sizeof(uint64_t), 1, f) != 1) { fclose(f); return false; }
    }

    // Extended metadata (v3+)
    if (version >= FORMAT_VERSION_V3) {
        uint32_t metaCount;
        if (fread(&metaCount, sizeof(uint32_t), 1, f) != 1) { fclose(f); return false; }
        for (uint32_t m = 0; m < metaCount; m++) {
            std::string key, value;
            if (!readString(f, key) || !readString(f, value)) { fclose(f); return false; }
            meta.extra[key] = value;
        }
    }

    // Record count
    uint32_t count;
    if (fread(&count, sizeof(uint32_t), 1, f) != 1) { fclose(f); return false; }

    std::vector<FileRecord> records;
    records.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        FileRecord r;
        if (!readRecordFromFile(f, r, version)) { fclose(f); return false; }
        records.push_back(std::move(r));
    }

    fclose(f);

    if (outMetadata) *outMetadata = std::move(meta);
    loadRecords(std::move(records));
    return true;
}

std::vector<uint32_t> SearchEngine::recentIndices(uint32_t count) const {
    std::shared_lock lock(mutex_);

    // Collect (index, modTime) for live records under a single lock
    std::vector<std::pair<uint32_t, time_t>> entries;
    entries.reserve(records_.size());
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type != 0) {
            entries.emplace_back(static_cast<uint32_t>(i), records_[i].modTime);
        }
    }

    uint32_t n = std::min(count, static_cast<uint32_t>(entries.size()));
    std::partial_sort(entries.begin(), entries.begin() + n, entries.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<uint32_t> result;
    result.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        result.push_back(entries[i].first);
    }
    return result;
}

uint32_t SearchEngine::indexForPath(const std::string& fullPath) const {
    std::shared_lock lock(mutex_);
    auto it = pathIndex_.find(toLower(fullPath));
    return (it != pathIndex_.end()) ? it->second : UINT32_MAX;
}

std::string SearchEngine::fullPathForRecord(const std::string& path, const std::string& name) {
    if (path.empty() || path.back() == '/') return path + name;
    return path + "/" + name;
}

// --- Legacy load overload ---
bool SearchEngine::loadFromFile(const std::string& filePath, uint64_t* outLastEventId) {
    IndexMetadata meta;
    bool ok = loadFromFile(filePath, &meta);
    if (ok && outLastEventId) *outLastEventId = meta.lastEventId;
    return ok;
}
