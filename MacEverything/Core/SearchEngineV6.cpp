#include "SearchEngine.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <unordered_set>
#include <thread>

// ---------------------------------------------------------------------------
// v6 Flat SoA: loadRecordsV6, completePhase2, snapshotForV6
// ---------------------------------------------------------------------------

void SearchEngine::loadRecordsV6(StringPool&& origNamePool,
                                  StringPool&& namePool,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathPool,
                                  StringPool&& lowerPathPool,
                                  std::vector<uint8_t>&& types,
                                  std::vector<uint64_t>&& sizes,
                                  std::vector<int64_t>&& modTimes,
                                  std::vector<uint64_t>&& inodes,
                                  std::vector<int32_t>&& devIds) {
    std::unique_lock lock(mutex_);

    uint32_t n = origNamePool.entryCount();

    // Install SoA columns directly (zero-copy from v6 file)
    origNamePool_ = std::move(origNamePool);
    namePool_ = std::move(namePool);
    pathIndices_ = std::move(pathIndices);
    pathPool_ = std::move(pathPool);
    lowerPathPool_ = std::move(lowerPathPool);
    types_ = std::move(types);
    sizes_ = std::move(sizes);
    modTimes_ = std::move(modTimes);
    inodes_ = std::move(inodes);
    devIds_ = std::move(devIds);

    // Reconstruct records_ from SoA columns + origNamePool
    records_.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        records_[i].name = origNamePool_.str(i);
        records_[i].type = types_[i];
        records_[i].size = sizes_[i];
        records_[i].modTime = static_cast<time_t>(modTimes_[i]);
        records_[i].inode = inodes_[i];
        records_[i].devId = devIds_[i];
        // records_[i].path left empty — resolved via pathPool_[pathIndices_[i]]
    }

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
    size_t chunkSize = (n + numThreads - 1) / numThreads;
    std::vector<std::string> loweredPaths(n);
    {
        std::vector<std::thread> pathThreads;
        pathThreads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, static_cast<size_t>(n));
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
    pathIndex_.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        pathIndex_[std::move(loweredPaths[i])] = i;
    }

    // Tombstone orphaned duplicates
    std::unordered_set<uint32_t> winnerIndices;
    winnerIndices.reserve(pathIndex_.size());
    for (const auto& [_, idx] : pathIndex_) {
        winnerIndices.insert(idx);
    }
    uint32_t actualLive = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (records_[i].type == 0) continue;
        if (winnerIndices.count(i)) {
            actualLive++;
        } else {
            tombstoneAt(i);
        }
    }

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Phase 2: defer trigram index building to background
    phase2Pending_.store(true, std::memory_order_release);
    phase2StartRecordCount_ = static_cast<uint32_t>(records_.size());

    // Initialize dirty page bitmap
    uint32_t pageCount = (n + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);

    LOG_INFO("SearchEngine", "loadRecordsV6: loaded " << n << " records (" << actualLive
             << " live), Phase 2 pending");
}

void SearchEngine::completePhase2() {
    if (!phase2Pending_.load(std::memory_order_acquire)) return;

    LOG_INFO("SearchEngine", "Phase 2: building trigram indices in background...");

    // Snapshot data needed for building indices (under shared lock)
    std::vector<FileRecord> snapRecords;
    StringPool snapNamePool;
    StringPool snapLowerPathPool;
    std::vector<uint32_t> snapPathIndices;
    uint32_t snapPathPoolSize;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        snapRecords = records_;
        snapNamePool = namePool_;
        snapLowerPathPool = lowerPathPool_;
        snapPathIndices = pathIndices_;
        snapPathPoolSize = pathPool_.entryCount();
        snapSize = static_cast<uint32_t>(records_.size());
    }

    // Build all indices without holding any lock (~3s)
    auto trigramIndex = buildTrigramIndexFromData(snapRecords, snapNamePool);
    auto pathTrigramIndex = buildPathTrigramIndexFromData(snapLowerPathPool);
    auto pathIdxToRecords = buildPathIdxToRecordsFromData(snapRecords, snapPathIndices, snapPathPoolSize);
    auto recentCache = buildRecentCacheFromData(snapRecords, kRecentCacheSize);

    LOG_INFO("SearchEngine", "Phase 2: indices built, swapping under lock...");

    // Swap under unique lock and replay mutations that occurred during build
    {
        std::unique_lock lock(mutex_);

        nameTrigramIndex_ = std::move(trigramIndex);
        pathTrigramIndex_ = std::move(pathTrigramIndex);
        pathIdxToRecords_ = std::move(pathIdxToRecords);
        recentCache_ = std::move(recentCache);

        // Replay records added during Phase 2 build
        uint32_t currentSize = static_cast<uint32_t>(records_.size());
        uint32_t replayCount = 0;
        for (uint32_t i = snapSize; i < currentSize; i++) {
            if (records_[i].type == 0) continue;
            // Add trigrams for this record
            addTrigramsForRecord(i, namePool_.data(i), namePool_.length(i));
            addPathTrigramsForRecord(i);
            addToRecentCache(i, records_[i].modTime);
            replayCount++;
        }

        // Replay tombstones: records that were live in snapshot but deleted during build
        for (uint32_t i = 0; i < snapSize; i++) {
            if (i >= records_.size()) break;
            if (snapRecords[i].type != 0 && records_[i].type == 0) {
                // Was live in snapshot, now tombstoned — trigram was built, need to remove
                // The trigram entry points at index i which is now tombstoned.
                // Query-time type==0 check will filter it, but we can clean up explicitly.
                removeTrigramsForRecord(i);  // Safe: namePool_[i] is tombstoned, this is a no-op
            }
        }

        phase2Pending_.store(false, std::memory_order_release);

        LOG_INFO("SearchEngine", "Phase 2 complete: replayed " << replayCount
                 << " mutations, trigram indices active");
    }
}

SearchEngine::V6Snapshot SearchEngine::snapshotForV6() const {
    std::shared_lock lock(mutex_);
    V6Snapshot snap;
    snap.records = records_;
    snap.origNamePool = origNamePool_;
    snap.namePool = namePool_;
    snap.pathIndices = pathIndices_;
    snap.pathPool = pathPool_;
    snap.lowerPathPool = lowerPathPool_;
    snap.types = types_;
    snap.sizes = sizes_;
    snap.modTimes = modTimes_;
    snap.inodes = inodes_;
    snap.devIds = devIds_;
    snap.liveCount = liveCount_.load(std::memory_order_relaxed);
    return snap;
}
