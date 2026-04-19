#include "SearchEngine.h"
#include "StringUtils.h"
#include "IndexWAL.h"
#include "Logger.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_set>

std::string SearchEngine::makeFullPath(const std::string& path, const std::string& name) {
    if (path.empty() || path.back() == '/') return path + name;
    return path + "/" + name;
}

uint32_t SearchEngine::internPath(const std::string& path) {
    auto it = pathLookup_.find(path);
    if (it != pathLookup_.end()) return it->second;
    uint32_t pIdx = pathPool_.append(path);
    lowerPathPool_.append(me::toLower(path));
    pathLookup_[path] = pIdx;
    lowerPathLookup_[me::toLower(path)] = pIdx;
    return pIdx;
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    std::unique_lock lock(mutex_);

    records_ = std::move(records);

    // Pre-compute lowercase names into a temporary vector (parallel),
    // then bulk-load into namePool_ (single-threaded append)
    std::vector<std::string> tempLowerNames(records_.size());
    {
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
            threads.emplace_back([this, &tempLowerNames, start, end] {
                for (size_t i = start; i < end; i++) {
                    tempLowerNames[i] = me::toLower(records_[i].name);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    namePool_.loadBulk(tempLowerNames);

    // Path deduplication: intern paths into pathPool_ + lowerPathPool_ + pathLookup_
    pathPool_.clear();
    lowerPathPool_.clear();
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathIndices_.resize(records_.size());
    for (size_t i = 0; i < records_.size(); i++) {
        pathIndices_[i] = internPath(records_[i].path);
        records_[i].path.clear();
    }

    // Build pathIndex_ (lowercase full path -> record index)
    // Parallelize toLower computation, insert into map sequentially
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;
    size_t chunkSize = (records_.size() + numThreads - 1) / numThreads;
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
                    loweredPaths[i] = makeFullPath(lowerPathPool_.str(pathIndices_[i]),
                                                   std::string(namePool_.data(static_cast<uint32_t>(i)),
                                                               namePool_.length(static_cast<uint32_t>(i))));
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

    // Tombstone orphaned duplicates: records whose index doesn't match pathIndex_
    std::unordered_set<uint32_t> winnerIndices;
    winnerIndices.reserve(pathIndex_.size());
    for (const auto& [_, idx] : pathIndex_) {
        winnerIndices.insert(idx);
    }
    uint32_t actualLive = 0;
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue;
        if (winnerIndices.count(static_cast<uint32_t>(i))) {
            actualLive++;
        } else {
            records_[i].type = 0;
            records_[i].name.clear();
            records_[i].size = 0;
            records_[i].modTime = 0;
            namePool_.tombstone(static_cast<uint32_t>(i));
        }
    }

    // Build trigram index for fast filename search
    buildTrigramIndex();
    // Build path trigram index for fast path-only search
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap (no pages dirty after initial load)
    uint32_t pageCount = (static_cast<uint32_t>(records_.size()) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

void SearchEngine::loadRecordsV5(std::vector<FileRecord>&& records,
                                  std::vector<std::string>&& lowerNames,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathDict,
                                  StringPool&& lowerPathDict) {
    std::unique_lock lock(mutex_);

    records_ = std::move(records);

    // Install pre-lowered names directly into namePool_ (skip parallel toLower)
    namePool_.loadBulk(lowerNames);

    // Install pre-built path dictionaries (skip path dedup + internPath loop)
    pathPool_ = std::move(pathDict);
    lowerPathPool_ = std::move(lowerPathDict);
    pathIndices_ = std::move(pathIndices);

    // Rebuild pathLookup_ and lowerPathLookup_ from pathPool_ entries
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathLookup_.reserve(pathPool_.entryCount());
    lowerPathLookup_.reserve(pathPool_.entryCount());
    for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
        if (pathPool_.isLive(i)) {
            pathLookup_[pathPool_.str(i)] = i;
            lowerPathLookup_[lowerPathPool_.str(i)] = i;
        }
    }

    // Build pathIndex_ (lowercase full path -> record index)
    // Parallelize toLower computation of full paths
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;
    size_t chunkSize = (records_.size() + numThreads - 1) / numThreads;
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
                    loweredPaths[i] = makeFullPath(lowerPathPool_.str(pathIndices_[i]),
                                                    std::string(namePool_.data(static_cast<uint32_t>(i)),
                                                                namePool_.length(static_cast<uint32_t>(i))));
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

    // Tombstone orphaned duplicates
    std::unordered_set<uint32_t> winnerIndices;
    winnerIndices.reserve(pathIndex_.size());
    for (const auto& [_, idx] : pathIndex_) {
        winnerIndices.insert(idx);
    }
    uint32_t actualLive = 0;
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue;
        if (winnerIndices.count(static_cast<uint32_t>(i))) {
            actualLive++;
        } else {
            records_[i].type = 0;
            records_[i].name.clear();
            records_[i].size = 0;
            records_[i].modTime = 0;
            namePool_.tombstone(static_cast<uint32_t>(i));
        }
    }

    // Build trigram index for fast filename search
    buildTrigramIndex();
    buildPathTrigramIndex();
    rebuildPathIdxToRecords();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap
    uint32_t pageCount = (static_cast<uint32_t>(records_.size()) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= records_.size()) return {};
    FileRecord r = records_[index];
    if (r.type != 0 && index < pathIndices_.size()) {
        r.path = pathPool_.str(pathIndices_[index]);
    }
    return r;
}

uint32_t SearchEngine::recordCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(records_.size());
}

uint32_t SearchEngine::addRecord(FileRecord&& record) {
    std::unique_lock lock(mutex_);

    uint32_t idx = static_cast<uint32_t>(records_.size());
    std::string fullPath = makeFullPath(record.path, record.name);
    std::string lowerFull = me::toLower(fullPath);
    std::string lower = me::toLower(record.name);

    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    // Tombstone existing record at same path to prevent orphaned duplicates
    auto existIt = pathIndex_.find(lowerFull);
    if (existIt != pathIndex_.end()) {
        uint32_t oldIdx = existIt->second;
        time_t oldModTime = records_[oldIdx].modTime;
        removeTrigramsForRecord(oldIdx);
        removePathTrigramsForRecord(oldIdx);
        records_[oldIdx].type = 0;
        markPageDirty(oldIdx);
        records_[oldIdx].name.clear();
        records_[oldIdx].size = 0;
        records_[oldIdx].modTime = 0;
        namePool_.tombstone(oldIdx);
        pathIndex_.erase(existIt);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(oldIdx, oldModTime);
    }

    // Intern path before moving record
    uint32_t pIdx = internPath(record.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    record.path.clear();
    record.path.shrink_to_fit();

    records_.push_back(std::move(record));
    uint32_t nameIdx = namePool_.append(lower);
    (void)nameIdx; // nameIdx == idx since namePool_ grows in lockstep
    pathIndices_.push_back(pIdx);
    pathIndex_[lowerFull] = idx;

    // Update trigram index
    addTrigramsForRecord(idx, namePool_.data(idx), namePool_.length(idx));
    addPathTrigramsForRecord(idx);

    // Dirty page tracking
    if (idx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
    }
    markPageDirty(idx);

    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(idx, records_[idx].modTime);

    return idx;
}

bool SearchEngine::removeByPathUnlocked(const std::string& fullPath) {
    auto it = pathIndex_.find(me::toLower(fullPath));
    if (it == pathIndex_.end()) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    uint32_t idx = it->second;
    time_t oldModTime = records_[idx].modTime;
    removeTrigramsForRecord(idx);
    removePathTrigramsForRecord(idx);
    records_[idx].type = 0;
    markPageDirty(idx);
    records_[idx].name.clear();
    records_[idx].size = 0;
    records_[idx].modTime = 0;
    namePool_.tombstone(idx);
    pathIndex_.erase(it);

    liveCount_.fetch_sub(1, std::memory_order_relaxed);
    removeFromRecentCache(idx, oldModTime);

    return true;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    std::unique_lock lock(mutex_);
    return removeByPathUnlocked(fullPath);
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    std::unique_lock lock(mutex_);

    std::string lowerPrefix = me::toLower(pathPrefix);
    uint32_t removed = 0;
    for (auto it = pathIndex_.begin(); it != pathIndex_.end(); ) {
        const auto& path = it->first;
        if (path.size() >= lowerPrefix.size() &&
            path.compare(0, lowerPrefix.size(), lowerPrefix) == 0 &&
            (path.size() == lowerPrefix.size() || path[lowerPrefix.size()] == '/')) {
            uint32_t idx = it->second;

            // H-4: Write WAL entry for each removed path
            if (wal_) {
                std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), records_[idx].name);
                wal_->append(WALOp::Remove, fullPath);
            }

            time_t oldModTime = records_[idx].modTime;
            // Clean up trigram index (must happen before clearing namePool_)
            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            records_[idx].type = 0;
            markPageDirty(idx);
            records_[idx].name.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            namePool_.tombstone(idx);
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

uint32_t SearchEngine::batchRescanPrefix(const std::string& pathPrefix,
                                         std::vector<FileRecord>&& freshRecords) {
    std::unique_lock lock(mutex_);

    // ── Phase 1: Tombstone old records matching prefix ──
    // Remove trigrams incrementally for each tombstoned record.
    std::string lowerPrefix = me::toLower(pathPrefix);
    uint32_t removed = 0;
    for (auto it = pathIndex_.begin(); it != pathIndex_.end(); ) {
        const auto& path = it->first;
        if (path.size() >= lowerPrefix.size() &&
            path.compare(0, lowerPrefix.size(), lowerPrefix) == 0 &&
            (path.size() == lowerPrefix.size() || path[lowerPrefix.size()] == '/')) {
            uint32_t idx = it->second;

            // Write WAL Remove entry for each removed path
            if (wal_) {
                std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), records_[idx].name);
                wal_->append(WALOp::Remove, fullPath);
            }

            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            records_[idx].type = 0;
            markPageDirty(idx);
            records_[idx].name.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            namePool_.tombstone(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            it = pathIndex_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    // ── Phase 2: Add fresh records with incremental trigram insertion ──
    for (auto& record : freshRecords) {
        uint32_t newIdx = static_cast<uint32_t>(records_.size());
        std::string fullPath = makeFullPath(record.path, record.name);
        std::string lower = me::toLower(record.name);

        if (wal_) wal_->append(WALOp::Update, fullPath, record);

        uint32_t pIdx = internPath(record.path);
        if (pIdx >= pathIdxToRecords_.size()) {
            ensurePathTrigramsForPathIdx(pIdx);
        }
        record.path.clear();
        record.path.shrink_to_fit();

        records_.push_back(std::move(record));
        namePool_.append(lower);
        pathIndices_.push_back(pIdx);
        pathIndex_[me::toLower(fullPath)] = newIdx;
        addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
        addPathTrigramsForRecord(newIdx);
        if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
            dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
        }
        markPageDirty(newIdx);
        liveCount_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Phase 3: Rebuild recent cache ──
    rebuildRecentCache();

    return removed;
}

void SearchEngine::updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated) {
    if (wal_) wal_->append(WALOp::Update, fullPath, updated);

    // Remove old record if exists (case-insensitive lookup)
    auto it = pathIndex_.find(me::toLower(fullPath));
    if (it != pathIndex_.end()) {
        uint32_t idx = it->second;
        time_t oldModTime = records_[idx].modTime;
        // Clean up trigram index (must happen before tombstoning namePool_)
        removeTrigramsForRecord(idx);
        removePathTrigramsForRecord(idx);
        records_[idx].type = 0;
        markPageDirty(idx);
        records_[idx].name.clear();
        records_[idx].size = 0;
        records_[idx].modTime = 0;
        namePool_.tombstone(idx);
        pathIndex_.erase(it);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(idx, oldModTime);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(records_.size());
    std::string newFullPath = makeFullPath(updated.path, updated.name);
    std::string lower = me::toLower(updated.name);

    // Intern path before moving record
    uint32_t pIdx = internPath(updated.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    updated.path.clear();
    updated.path.shrink_to_fit();

    records_.push_back(std::move(updated));
    namePool_.append(lower);
    pathIndices_.push_back(pIdx);
    pathIndex_[me::toLower(newFullPath)] = newIdx;
    addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
    addPathTrigramsForRecord(newIdx);
    if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
    }
    markPageDirty(newIdx);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(newIdx, records_[newIdx].modTime);
}

void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    std::unique_lock lock(mutex_);
    updateByPathUnlocked(fullPath, std::move(updated));
}

void SearchEngine::batchMutate(std::vector<MutationOp>&& ops) {
    if (ops.empty()) return;
    std::unique_lock lock(mutex_);
    for (auto& op : ops) {
        if (op.type == MutationOp::REMOVE) {
            removeByPathUnlocked(op.path);
        } else {
            updateByPathUnlocked(op.path, std::move(op.record));
        }
    }
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    // ── Phase 1: Snapshot under shared_lock ──
    // Queries continue unblocked during snapshot copy.
    std::vector<FileRecord> snapRecords;
    StringPool snapNamePool;
    std::vector<uint32_t> snapPathIndices;
    StringPool snapPathPool;
    std::unordered_map<std::string, uint32_t> snapPathIndex;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        uint32_t live = liveCount_.load(std::memory_order_relaxed);
        if (live == records_.size()) return {}; // nothing to compact
        snapRecords = records_;
        snapNamePool = namePool_;
        snapPathIndices = pathIndices_;
        snapPathPool = pathPool_;
        snapPathIndex = pathIndex_;
        snapSize = static_cast<uint32_t>(records_.size());
    }

    LOG_INFO("SearchEngine", "COW compaction Phase 2: building compacted data from "
             << snapSize << " records");

    // ── Phase 2: Build compacted data (no lock held) ──
    // Mutations (addRecord/removeByPath/updateByPath/batchRescanPrefix) continue
    // on the live data; they will be replayed in Phase 3.
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(snapSize);

    std::vector<FileRecord> cdRecords;
    cdRecords.reserve(snapSize);
    StringPool cdNamePool;
    std::vector<uint32_t> cdPathIndices;
    cdPathIndices.reserve(snapSize);
    StringPool cdPathPool;
    StringPool cdLowerPathPool;
    std::unordered_map<std::string, uint32_t> cdPathLookup;
    std::unordered_map<std::string, uint32_t> cdLowerPathLookup;
    std::unordered_map<std::string, uint32_t> cdPathIndex;
    cdPathIndex.reserve(snapSize);

    for (size_t i = 0; i < snapSize; i++) {
        if (snapRecords[i].type == 0) continue;
        // Skip orphaned duplicates: live records not referenced by pathIndex_
        std::string origPath = snapPathPool.str(snapPathIndices[i]);
        std::string fullPathLower = me::toLower(makeFullPath(origPath, snapRecords[i].name));
        auto pathIt = snapPathIndex.find(fullPathLower);
        if (pathIt == snapPathIndex.end() || pathIt->second != static_cast<uint32_t>(i)) continue;
        uint32_t newIdx = static_cast<uint32_t>(cdRecords.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        // Intern path into compacted pool (both original and lowered)
        uint32_t newPIdx;
        auto cdPlIt = cdPathLookup.find(origPath);
        if (cdPlIt != cdPathLookup.end()) {
            newPIdx = cdPlIt->second;
        } else {
            newPIdx = cdPathPool.append(origPath);
            cdLowerPathPool.append(me::toLower(origPath));
            cdPathLookup[origPath] = newPIdx;
            cdLowerPathLookup[me::toLower(origPath)] = newPIdx;
        }
        cdPathIndex[fullPathLower] = newIdx;
        // Copy name from snapshot pool into compacted pool
        cdNamePool.append(snapNamePool.data(i), snapNamePool.length(i));
        cdPathIndices.push_back(newPIdx);
        cdRecords.push_back(std::move(snapRecords[i]));
    }
    uint32_t cdLiveCount = static_cast<uint32_t>(cdRecords.size());

    // Build trigram index, path trigram index, and recent cache outside any lock
    auto cdTrigramIndex = buildTrigramIndexFromData(cdRecords, cdNamePool);
    auto cdPathTrigramIndex = buildPathTrigramIndexFromData(cdLowerPathPool);
    auto cdPathIdxToRecords = buildPathIdxToRecordsFromData(cdRecords, cdPathIndices, cdPathPool.entryCount());
    auto cdRecentCache = buildRecentCacheFromData(cdRecords, kRecentCacheSize);

    LOG_INFO("SearchEngine", "COW compaction Phase 3: swapping data, compacted "
             << snapSize << " -> " << cdLiveCount << " records");

    // ── Phase 3: Swap + replay under unique_lock ──
    // This lock is held only long enough to move data and replay the small
    // number of mutations that occurred during Phase 2 (~milliseconds).
    {
        std::unique_lock lock(mutex_);

        // Move out current (mutated-during-Phase2) state
        auto oldRecords = std::move(records_);
        auto oldNamePool = std::move(namePool_);
        auto oldPathIndices = std::move(pathIndices_);
        auto oldPathPool = std::move(pathPool_);
        auto oldPathLookup = std::move(pathLookup_);
        auto oldLowerPathLookup = std::move(lowerPathLookup_);
        auto oldPathIndex = std::move(pathIndex_);

        // Install compacted data
        records_ = std::move(cdRecords);
        namePool_ = std::move(cdNamePool);
        pathIndices_ = std::move(cdPathIndices);
        pathPool_ = std::move(cdPathPool);
        lowerPathPool_ = std::move(cdLowerPathPool);
        pathLookup_ = std::move(cdPathLookup);
        lowerPathLookup_ = std::move(cdLowerPathLookup);
        pathIndex_ = std::move(cdPathIndex);
        nameTrigramIndex_ = std::move(cdTrigramIndex);
        pathTrigramIndex_ = std::move(cdPathTrigramIndex);
        pathIdxToRecords_ = std::move(cdPathIdxToRecords);
        recentCache_ = std::move(cdRecentCache);

        // Replay new records appended during Phase 2
        uint32_t replayedAdds = 0;
        for (size_t i = snapSize; i < oldRecords.size(); i++) {
            if (oldRecords[i].type == 0) continue;
            uint32_t newIdx = static_cast<uint32_t>(records_.size());
            std::string origPath = oldPathPool.str(oldPathIndices[i]);
            uint32_t pIdx = internPath(origPath);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            std::string fullPath = me::toLower(makeFullPath(origPath, oldRecords[i].name));
            // Copy name from old pool into current pool
            namePool_.append(oldNamePool.data(i), oldNamePool.length(i));
            pathIndex_[fullPath] = newIdx;
            pathIndices_.push_back(pIdx);
            addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
            addToRecentCache(newIdx, oldRecords[i].modTime);
            records_.push_back(std::move(oldRecords[i]));
            addPathTrigramsForRecord(newIdx);
            cdLiveCount++;
            replayedAdds++;
        }

        // Replay tombstones: paths in snapshot but removed during Phase 2
        uint32_t replayedDeletes = 0;
        for (auto& [path, snapIdx] : snapPathIndex) {
            if (oldPathIndex.find(path) != oldPathIndex.end()) continue;
            // This path was deleted during Phase 2
            auto it = remap.find(snapIdx);
            if (it == remap.end()) continue; // was already tombstoned in snapshot
            uint32_t newIdx = it->second;
            if (newIdx < records_.size() && records_[newIdx].type != 0) {
                removeTrigramsForRecord(newIdx);
                removePathTrigramsForRecord(newIdx);
                time_t oldMod = records_[newIdx].modTime;
                records_[newIdx].type = 0;
                records_[newIdx].name.clear();
                records_[newIdx].size = 0;
                records_[newIdx].modTime = 0;
                namePool_.tombstone(newIdx);
                pathIndex_.erase(path);
                removeFromRecentCache(newIdx, oldMod);
                cdLiveCount--;
                replayedDeletes++;
            }
        }

        liveCount_.store(cdLiveCount, std::memory_order_relaxed);
        compactionGen_.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO("SearchEngine", "COW compaction done: replayed " << replayedAdds
                 << " adds, " << replayedDeletes << " deletes, live=" << cdLiveCount);
    }

    fullRewriteNeeded_.store(true, std::memory_order_relaxed);

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

std::set<SearchEngine::RecentEntry>
SearchEngine::buildRecentCacheFromData(const std::vector<FileRecord>& records,
                                       uint32_t cacheSize) {
    struct TimePair { time_t modTime; uint32_t index; };
    std::vector<TimePair> pairs;
    pairs.reserve(records.size());
    for (size_t i = 0; i < records.size(); i++) {
        if (records[i].type == 0) continue;
        pairs.push_back({records[i].modTime, static_cast<uint32_t>(i)});
    }
    std::set<RecentEntry> cache;
    size_t k = std::min(pairs.size(), static_cast<size_t>(cacheSize));
    if (k > 0) {
        std::partial_sort(pairs.begin(), pairs.begin() + k, pairs.end(),
                          [](const TimePair& a, const TimePair& b) { return a.modTime > b.modTime; });
        for (size_t i = 0; i < k; i++) {
            cache.insert({pairs[i].modTime, pairs[i].index});
        }
    }
    return cache;
}

void SearchEngine::rebuildRecentCache() {
    recentCache_ = buildRecentCacheFromData(records_, kRecentCacheSize);
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
    auto it = pathIndex_.find(me::toLower(fullPath));
    return (it != pathIndex_.end()) ? it->second : UINT32_MAX;
}

std::vector<FileRecord> SearchEngine::exportRecords() const {
    std::shared_lock lock(mutex_);
    std::vector<FileRecord> result;
    result.reserve(liveCount_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < records_.size(); i++) {
        if (records_[i].type == 0) continue; // skip tombstones
        FileRecord r = records_[i];
        r.path = pathPool_.str(pathIndices_[i]);
        result.push_back(std::move(r));
    }
    return result;
}

void SearchEngine::replayWALEntries(std::vector<WALEntry>&& entries) {
    if (entries.empty()) return;

    std::unique_lock lock(mutex_);

    for (auto& e : entries) {
        std::string lowerFull = me::toLower(e.fullPath);

        switch (e.op) {
        case WALOp::Add: {
            // If path already exists (duplicate Add), tombstone old record first
            auto existIt = pathIndex_.find(lowerFull);
            if (existIt != pathIndex_.end()) {
                uint32_t oldIdx = existIt->second;
                removeTrigramsForRecord(oldIdx);
                removePathTrigramsForRecord(oldIdx);
                records_[oldIdx].type = 0;
                markPageDirty(oldIdx);
                records_[oldIdx].name.clear();
                records_[oldIdx].size = 0;
                records_[oldIdx].modTime = 0;
                namePool_.tombstone(oldIdx);
                pathIndex_.erase(existIt);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }

            uint32_t idx = static_cast<uint32_t>(records_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            records_.push_back(std::move(e.record));
            namePool_.append(lower);
            pathIndices_.push_back(pIdx);
            pathIndex_[lowerFull] = idx;
            addTrigramsForRecord(idx, namePool_.data(idx), namePool_.length(idx));
            addPathTrigramsForRecord(idx);
            if (idx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
            }
            markPageDirty(idx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Remove: {
            auto it = pathIndex_.find(lowerFull);
            if (it == pathIndex_.end()) break; // silently ignore
            uint32_t idx = it->second;
            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            records_[idx].type = 0;
            markPageDirty(idx);
            records_[idx].name.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            namePool_.tombstone(idx);
            pathIndex_.erase(it);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Update: {
            // Remove old record if exists
            auto it = pathIndex_.find(lowerFull);
            if (it != pathIndex_.end()) {
                uint32_t oldIdx = it->second;
                removeTrigramsForRecord(oldIdx);
                removePathTrigramsForRecord(oldIdx);
                records_[oldIdx].type = 0;
                markPageDirty(oldIdx);
                records_[oldIdx].name.clear();
                records_[oldIdx].size = 0;
                records_[oldIdx].modTime = 0;
                namePool_.tombstone(oldIdx);
                pathIndex_.erase(it);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }
            // Add updated record
            uint32_t newIdx = static_cast<uint32_t>(records_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            records_.push_back(std::move(e.record));
            namePool_.append(lower);
            pathIndices_.push_back(pIdx);
            pathIndex_[lowerFull] = newIdx;
            addTrigramsForRecord(newIdx, namePool_.data(newIdx), namePool_.length(newIdx));
            addPathTrigramsForRecord(newIdx);
            if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
            }
            markPageDirty(newIdx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        }
    }

    rebuildRecentCache();
}
