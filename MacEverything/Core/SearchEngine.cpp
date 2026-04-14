#include "SearchEngine.h"
#include "IndexWAL.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <unordered_set>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

std::string SearchEngine::toLower(const std::string& s) {
    // H1 fix: ASCII fast-path — skip CoreFoundation for pure-ASCII strings
    // (>95% of filenames on typical systems)
    bool allAscii = true;
    for (unsigned char c : s) {
        if (c >= 128) { allAscii = false; break; }
    }
    if (allAscii) {
        std::string result(s.size(), '\0');
        for (size_t i = 0; i < s.size(); i++)
            result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        return result;
    }

    // Unicode-aware lowercasing via CoreFoundation (non-ASCII only)
    CFStringRef cfStr = CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(s.data()), static_cast<CFIndex>(s.size()),
        kCFStringEncodingUTF8, false);
    if (!cfStr) {
        std::string result(s.size(), '\0');
        for (size_t i = 0; i < s.size(); i++)
            result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        return result;
    }
    CFMutableStringRef mutable_ = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, cfStr);
    CFRelease(cfStr);
    CFStringLowercase(mutable_, CFLocaleGetSystem());

    CFIndex len = CFStringGetLength(mutable_);
    CFIndex maxBuf = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(maxBuf), '\0');
    CFStringGetCString(mutable_, result.data(), maxBuf, kCFStringEncodingUTF8);
    CFRelease(mutable_);
    result.resize(std::strlen(result.c_str()));
    return result;
}

std::string SearchEngine::makeFullPath(const std::string& path, const std::string& name) {
    if (path.empty() || path.back() == '/') return path + name;
    return path + "/" + name;
}

void SearchEngine::buildTrigramIndex() {
    nameTrigramIndex_.clear();
    recordTrigrams_.resize(lowerNames_.size());

    for (size_t i = 0; i < lowerNames_.size(); i++) {
        if (records_[i].type == 0) {
            recordTrigrams_[i].clear();
            continue;
        }
        auto trigrams = ContentIndex::extractTrigrams(lowerNames_[i]);
        // P-2 fix: sort+unique instead of unordered_set to reduce heap allocations
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        recordTrigrams_[i] = std::move(trigrams);
        for (Trigram t : recordTrigrams_[i]) {
            nameTrigramIndex_[t].push_back(static_cast<uint32_t>(i));
        }
    }

    // Sort posting lists for efficient set_intersection during query
    for (auto& [trigram, list] : nameTrigramIndex_) {
        std::sort(list.begin(), list.end());
    }
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const std::string& lowerName) {
    auto trigrams = ContentIndex::extractTrigrams(lowerName);
    // P-2 fix: sort+unique instead of unordered_set
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    if (idx >= recordTrigrams_.size()) {
        recordTrigrams_.resize(idx + 1);
    }
    recordTrigrams_[idx] = std::move(trigrams);

    for (Trigram t : recordTrigrams_[idx]) {
        auto& list = nameTrigramIndex_[t];
        // Insert in sorted position to maintain sorted posting lists
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        list.insert(pos, idx);
    }
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= recordTrigrams_.size()) return;
    for (Trigram t : recordTrigrams_[idx]) {
        auto it = nameTrigramIndex_.find(t);
        if (it != nameTrigramIndex_.end()) {
            auto& list = it->second;
            // Binary search in sorted list
            auto pos = std::lower_bound(list.begin(), list.end(), idx);
            if (pos != list.end() && *pos == idx) {
                list.erase(pos);
            }
            if (list.empty()) {
                nameTrigramIndex_.erase(it);
            }
        }
    }
    recordTrigrams_[idx].clear();
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    std::unique_lock lock(mutex_);

    records_ = std::move(records);
    lowerNames_.resize(records_.size());

    // Parallelize lowercase name pre-computation
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
            }
        });
    }

    for (auto& th : threads) th.join();

    // H6 fix: Parallelize path string computation, insert into map sequentially
    // (map insertion is not thread-safe, but string construction is the bottleneck)
    std::vector<std::string> loweredPaths(records_.size());
    {
        std::vector<std::thread> pathThreads;
        pathThreads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, records_.size());
            if (start >= end) break;
            pathThreads.emplace_back([this, &loweredPaths, start, end] {
                for (size_t i = start; i < end; i++) {
                    loweredPaths[i] = toLower(makeFullPath(records_[i].path, records_[i].name));
                }
            });
        }
        for (auto& th : pathThreads) th.join();
    }
    pathIndex_.clear();
    pathIndex_.reserve(records_.size());
    for (size_t i = 0; i < records_.size(); i++) {
        pathIndex_[std::move(loweredPaths[i])] = static_cast<uint32_t>(i);
    }

    // Build trigram index for fast filename search
    buildTrigramIndex();
    rebuildRecentCache();

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
    // Increment generation so any in-flight query detects it has been superseded.
    // Must happen before the empty check so clearing the search box also cancels stale queries.
    uint64_t myGen = queryGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (keyword.empty()) return {};

    std::string lowerKey = toLower(keyword);
    bool useGlob = isGlobPattern(lowerKey);

    // C-1 fix: Hold shared_lock for the entire query to prevent use-after-free.
    // compactRecords() replaces records_/lowerNames_ via move-assign under unique_lock,
    // which would free the old storage. shared_lock prevents compaction during query.
    // dispatch_apply under shared_lock is safe — multiple readers can hold it concurrently.
    std::shared_lock lock(mutex_);

    if (records_.empty()) return {};
    size_t totalSize = records_.size();

    std::vector<uint32_t> trigramCandidates;
    bool useTrigramIndex = false;

    // --- Trigram-accelerated path for non-glob queries with keyword >= 3 chars ---
    useTrigramIndex = !useGlob && lowerKey.size() >= 3 && !nameTrigramIndex_.empty();

    if (useTrigramIndex) {
        auto keyTrigrams = ContentIndex::extractTrigrams(lowerKey);
        std::unordered_set<Trigram> uniqueKeyTrigrams(keyTrigrams.begin(), keyTrigrams.end());

        if (!uniqueKeyTrigrams.empty()) {
            // Collect posting lists sorted by size (smallest first)
            std::vector<const std::vector<uint32_t>*> postingLists;
            bool allFound = true;
            for (Trigram t : uniqueKeyTrigrams) {
                auto it = nameTrigramIndex_.find(t);
                if (it == nameTrigramIndex_.end()) {
                    allFound = false;
                    break;
                }
                postingLists.push_back(&it->second);
            }

            if (allFound && !postingLists.empty()) {
                std::sort(postingLists.begin(), postingLists.end(),
                    [](const auto* a, const auto* b) { return a->size() < b->size(); });

                // Copy the shortest list (small enough to copy)
                trigramCandidates = *postingLists[0];

                // Intersect with remaining lists using sorted merge
                for (size_t li = 1; li < postingLists.size() && !trigramCandidates.empty(); li++) {
                    const auto& other = *postingLists[li];
                    std::vector<uint32_t> intersection;
                    intersection.reserve(std::min(trigramCandidates.size(), other.size()));
                    std::set_intersection(trigramCandidates.begin(), trigramCandidates.end(),
                                          other.begin(), other.end(),
                                          std::back_inserter(intersection));
                    trigramCandidates = std::move(intersection);
                }
            }
        } else {
            useTrigramIndex = false;
        }
    }

    // Collect results: (index, priority, pathLen) so sorting doesn't need records_ access.
    // Priority: 0=name exact match, 1=name starts with, 2=name contains, 3=path-only match
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };
    std::vector<Match> merged;

    if (useTrigramIndex) {
        // Phase 1: Check trigram candidates for name matches (fast, only a subset)
        for (size_t ci = 0; ci < trigramCandidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            uint32_t idx = trigramCandidates[ci];
            if (records_[idx].type == 0) continue;
            const auto& lowerName = lowerNames_[idx];
            if (lowerName.find(lowerKey) != std::string::npos) {
                uint8_t priority;
                if (lowerName == lowerKey) {
                    priority = 0;
                } else if (lowerName.size() >= lowerKey.size() &&
                           lowerName.compare(0, lowerKey.size(), lowerKey) == 0) {
                    priority = 1;
                } else {
                    priority = 2;
                }
                uint32_t pLen = static_cast<uint32_t>(records_[idx].path.size() + 1 + records_[idx].name.size());
                merged.push_back({idx, priority, pLen});
            }
        }

        // Phase 2: Linear scan for path-only matches (skip if maxResults already satisfied)
        if (maxResults > 0 && merged.size() >= maxResults) {
            // Already have enough results from trigram name matches, skip path scan
        } else {
        // Use parallel scan for path matches
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

        std::vector<std::vector<Match>> threadResults(numThreads);
        auto* threadResultsPtr = &threadResults;

        // C-1 fix: Access records_/lowerNames_ directly — shared_lock held at function scope
        // prevents compactRecords() from replacing these vectors.
        const auto& records = records_;
        const auto& lowerNames = lowerNames_;

        // Build a set of name-matched indices to skip (trigramCandidates is sorted)
        const auto* candidatesPtr = &trigramCandidates;

        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        const auto* genPtr = &queryGeneration_;
        uint64_t capturedGen = myGen;
        dispatch_apply(numThreads, queue, ^(size_t t) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, totalSize);
            if (start >= end) return;

            auto& local = (*threadResultsPtr)[t];
            for (size_t i = start; i < end; i++) {
                if ((i & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
                if (records[i].type == 0) continue;
                // Skip indices already handled by trigram phase
                if (std::binary_search(candidatesPtr->begin(), candidatesPtr->end(), static_cast<uint32_t>(i))) continue;

                // Compute lowercase full path on-the-fly (avoids storing lowerPaths_ vector)
                std::string lowerPath = toLower(makeFullPath(records[i].path, records[i].name));
                if (lowerPath.find(lowerKey) != std::string::npos) {
                    const auto& lowerName = lowerNames[i];
                    uint32_t pLen = static_cast<uint32_t>(records[i].path.size() + 1 + records[i].name.size());
                    if (lowerName.find(lowerKey) != std::string::npos) {
                        uint8_t priority;
                        if (lowerName == lowerKey) priority = 0;
                        else if (lowerName.size() >= lowerKey.size() &&
                                 lowerName.compare(0, lowerKey.size(), lowerKey) == 0) priority = 1;
                        else priority = 2;
                        local.push_back({static_cast<uint32_t>(i), priority, pLen});
                    } else {
                        local.push_back({static_cast<uint32_t>(i), uint8_t(3), pLen});
                    }
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
        }
        } // end Phase 2 maxResults check
    } else {
        // Original linear scan path (for glob patterns or short keywords)
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

        std::vector<std::vector<Match>> threadResults(numThreads);

        auto* threadResultsPtr = &threadResults;
        // C-1 fix: Access members directly — shared_lock held at function scope
        const auto& records = records_;
        const auto& lowerNames = lowerNames_;

        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        const auto* genPtr = &queryGeneration_;
        uint64_t capturedGen = myGen;
        dispatch_apply(numThreads, queue, ^(size_t t) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, totalSize);
            if (start >= end) return;

            auto& local = (*threadResultsPtr)[t];
            for (size_t i = start; i < end; i++) {
                if ((i & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
                if (records[i].type == 0) continue;

                const auto& lowerName = lowerNames[i];

                bool nameMatch = useGlob ? globMatch(lowerKey, lowerName)
                                         : (lowerName.find(lowerKey) != std::string::npos);
                bool pathMatch = false;
                if (!nameMatch) {
                    // Compute lowercase full path on-the-fly (avoids storing lowerPaths_ vector)
                    std::string lowerPath = toLower(makeFullPath(records[i].path, records[i].name));
                    pathMatch = useGlob ? globMatch(lowerKey, lowerPath)
                                        : (lowerPath.find(lowerKey) != std::string::npos);
                }

                if (nameMatch || pathMatch) {
                    uint8_t priority;
                    if (nameMatch) {
                        if (lowerName == lowerKey) priority = 0;
                        else if (lowerName.size() >= lowerKey.size() &&
                                 lowerName.compare(0, lowerKey.size(), lowerKey) == 0) priority = 1;
                        else priority = 2;
                    } else {
                        priority = 3;
                    }
                    uint32_t pLen = static_cast<uint32_t>(records[i].path.size() + 1 + records[i].name.size());
                    local.push_back({static_cast<uint32_t>(i), priority, pLen});
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
        }
    }

    // Final cancellation check before sorting
    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

    // C-1 fix: Release shared_lock before sorting — Match structs contain only
    // local data (idx, priority, pathLen), no references into records_/lowerNames_.
    lock.unlock();

    // Sort by priority, then by full path length (shorter = shallower = better)
    auto cmp = [](const Match& a, const Match& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.pathLen < b.pathLen;
    };

    size_t resultCount = merged.size();
    if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;

    if (resultCount < merged.size()) {
        std::partial_sort(merged.begin(), merged.begin() + resultCount, merged.end(), cmp);
    } else {
        std::sort(merged.begin(), merged.end(), cmp);
    }

    std::vector<uint32_t> result;
    result.reserve(resultCount);
    for (size_t i = 0; i < resultCount; i++) {
        result.push_back(merged[i].idx);
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

    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    records_.push_back(std::move(record));
    lowerNames_.push_back(lower);
    pathIndex_[toLower(fullPath)] = idx;

    // Update trigram index
    addTrigramsForRecord(idx, lower);

    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(idx, records_[idx].modTime);

    return idx;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    std::unique_lock lock(mutex_);

    auto it = pathIndex_.find(toLower(fullPath));
    if (it == pathIndex_.end()) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    uint32_t idx = it->second;
    time_t oldModTime = records_[idx].modTime;
    records_[idx].type = 0;
    records_[idx].name.clear();
    records_[idx].path.clear();
    records_[idx].size = 0;
    records_[idx].modTime = 0;
    lowerNames_[idx].clear();
    pathIndex_.erase(it);

    // Clean up trigram index
    removeTrigramsForRecord(idx);

    liveCount_.fetch_sub(1, std::memory_order_relaxed);
    removeFromRecentCache(idx, oldModTime);

    return true;
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    std::unique_lock lock(mutex_);

    std::string lowerPrefix = toLower(pathPrefix);
    uint32_t removed = 0;
    // Single-pass: iterate and erase matching entries using iterator advancement.
    // unordered_map::erase(iterator) returns the next valid iterator, so this is safe.
    // pathIndex_ keys are already lowercase.
    for (auto it = pathIndex_.begin(); it != pathIndex_.end(); ) {
        const auto& path = it->first;
        if (path.size() >= lowerPrefix.size() &&
            path.compare(0, lowerPrefix.size(), lowerPrefix) == 0 &&
            (path.size() == lowerPrefix.size() || path[lowerPrefix.size()] == '/')) {
            uint32_t idx = it->second;

            // H-4: Write WAL entry for each removed path
            if (wal_) {
                std::string fullPath = makeFullPath(records_[idx].path, records_[idx].name);
                wal_->append(WALOp::Remove, fullPath);
            }

            time_t oldModTime = records_[idx].modTime;
            records_[idx].type = 0;
            records_[idx].name.clear();
            records_[idx].path.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            lowerNames_[idx].clear();
            removeTrigramsForRecord(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            removeFromRecentCache(idx, oldModTime);
            it = pathIndex_.erase(it);
            removed++;
        } else {
            ++it;
        }
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
        time_t oldModTime = records_[idx].modTime;
        records_[idx].type = 0;
        records_[idx].name.clear();
        records_[idx].path.clear();
        records_[idx].size = 0;
        records_[idx].modTime = 0;
        lowerNames_[idx].clear();
        pathIndex_.erase(it);
        removeTrigramsForRecord(idx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(idx, oldModTime);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(records_.size());
    std::string newFullPath = makeFullPath(updated.path, updated.name);
    std::string lower = toLower(updated.name);

    records_.push_back(std::move(updated));
    lowerNames_.push_back(lower);
    pathIndex_[toLower(newFullPath)] = newIdx;
    addTrigramsForRecord(newIdx, lower);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(newIdx, records_[newIdx].modTime);
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
    std::unordered_map<std::string, uint32_t> newPathIndex;
    newPathIndex.reserve(live);

    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue;
        uint32_t newIdx = static_cast<uint32_t>(newRecords.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        std::string fullPath = toLower(makeFullPath(records_[i].path, records_[i].name));
        newPathIndex[fullPath] = newIdx;
        newLowerNames.push_back(std::move(lowerNames_[i]));
        newRecords.push_back(std::move(records_[i]));
    }

    records_ = std::move(newRecords);
    lowerNames_ = std::move(newLowerNames);
    pathIndex_ = std::move(newPathIndex);

    // Rebuild trigram index from scratch
    buildTrigramIndex();
    rebuildRecentCache();

    compactionGen_.fetch_add(1, std::memory_order_relaxed);

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

std::vector<uint32_t> SearchEngine::recentIndices(uint32_t count) const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    uint32_t n = std::min(count, static_cast<uint32_t>(recentCache_.size()));
    result.reserve(n);
    auto it = recentCache_.begin();
    for (uint32_t i = 0; i < n; ++i, ++it) {
        result.push_back(it->index);
    }
    return result;
}

void SearchEngine::rebuildRecentCache() {
    recentCache_.clear();
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue;
        recentCache_.insert({records_[i].modTime, static_cast<uint32_t>(i)});
        if (recentCache_.size() > kRecentCacheSize) {
            recentCache_.erase(std::prev(recentCache_.end()));
        }
    }
}

void SearchEngine::addToRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.insert({modTime, idx});
    if (recentCache_.size() > kRecentCacheSize) {
        recentCache_.erase(std::prev(recentCache_.end()));
    }
}

void SearchEngine::removeFromRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.erase({modTime, idx});
}

uint32_t SearchEngine::indexForPath(const std::string& fullPath) const {
    std::shared_lock lock(mutex_);
    auto it = pathIndex_.find(toLower(fullPath));
    return (it != pathIndex_.end()) ? it->second : UINT32_MAX;
}
