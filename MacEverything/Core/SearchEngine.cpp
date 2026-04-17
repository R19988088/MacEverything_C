#include "SearchEngine.h"
#include "StringUtils.h"
#include "IndexWAL.h"
#include "Logger.h"
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

std::string SearchEngine::makeFullPath(const std::string& path, const std::string& name) {
    if (path.empty() || path.back() == '/') return path + name;
    return path + "/" + name;
}

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildTrigramIndexFromData(const std::vector<FileRecord>& records,
                                        const std::vector<std::string>& lowerNames) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    for (size_t i = 0; i < lowerNames.size(); i++) {
        if (records[i].type == 0) continue;
        auto trigrams = ContentIndex::extractTrigrams(lowerNames[i]);
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        for (Trigram t : trigrams) {
            index[t].push_back(static_cast<uint32_t>(i));
        }
    }
    for (auto& [trigram, list] : index) {
        std::sort(list.begin(), list.end());
    }
    return index;
}

void SearchEngine::buildTrigramIndex() {
    nameTrigramIndex_ = buildTrigramIndexFromData(records_, lowerNames_);
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const std::string& lowerName) {
    auto trigrams = ContentIndex::extractTrigrams(lowerName);
    // P-2 fix: sort+unique instead of unordered_set
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    for (Trigram t : trigrams) {
        auto& list = nameTrigramIndex_[t];
        // Insert in sorted position to maintain sorted posting lists
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        list.insert(pos, idx);
    }
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= lowerNames_.size() || lowerNames_[idx].empty()) return;
    // Recompute trigrams from lowerNames_ instead of storing per-record lists
    auto trigrams = ContentIndex::extractTrigrams(lowerNames_[idx]);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
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
}

// --- Path trigram index methods ---

std::unordered_map<Trigram, std::vector<uint32_t>>
SearchEngine::buildPathTrigramIndexFromData(const PathTable& pathTable) {
    std::unordered_map<Trigram, std::vector<uint32_t>> index;
    for (uint32_t pi = 0; pi < pathTable.size(); pi++) {
        const auto& path = pathTable.resolve(pi);
        std::string lowerPath = me::toLower(path);
        auto trigrams = ContentIndex::extractTrigrams(lowerPath);
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        for (Trigram t : trigrams) {
            index[t].push_back(pi);
        }
    }
    for (auto& [_, list] : index) {
        std::sort(list.begin(), list.end());
    }
    return index;
}

std::vector<std::vector<uint32_t>>
SearchEngine::buildPathIdxToRecordsFromData(const std::vector<FileRecord>& records,
                                            const std::vector<uint32_t>& pathIndices,
                                            uint32_t pathTableSize) {
    std::vector<std::vector<uint32_t>> mapping(pathTableSize);
    for (size_t i = 0; i < records.size(); i++) {
        if (records[i].type == 0) continue;
        uint32_t pi = pathIndices[i];
        if (pi < mapping.size()) {
            mapping[pi].push_back(static_cast<uint32_t>(i));
        }
    }
    for (auto& list : mapping) {
        std::sort(list.begin(), list.end());
    }
    return mapping;
}

void SearchEngine::buildPathTrigramIndex() {
    pathTrigramIndex_ = buildPathTrigramIndexFromData(pathTable_);
}

void SearchEngine::rebuildPathIdxToRecords() {
    pathIdxToRecords_ = buildPathIdxToRecordsFromData(records_, pathIndices_, pathTable_.size());
}

void SearchEngine::addPathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    // Ensure pathIdxToRecords_ is large enough
    if (pi >= pathIdxToRecords_.size()) {
        pathIdxToRecords_.resize(pi + 1);
    }
    auto& list = pathIdxToRecords_[pi];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    list.insert(pos, idx);
}

void SearchEngine::removePathTrigramsForRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    uint32_t pi = pathIndices_[idx];
    if (pi >= pathIdxToRecords_.size()) return;
    auto& list = pathIdxToRecords_[pi];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    if (pos != list.end() && *pos == idx) {
        list.erase(pos);
    }
}

void SearchEngine::ensurePathTrigramsForPathIdx(uint32_t pathIdx) {
    // Check if this pathIdx already has entries in pathTrigramIndex_
    const auto& path = pathTable_.resolve(pathIdx);
    std::string lowerPath = me::toLower(path);
    auto trigrams = ContentIndex::extractTrigrams(lowerPath);
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        auto& list = pathTrigramIndex_[t];
        auto pos = std::lower_bound(list.begin(), list.end(), pathIdx);
        if (pos == list.end() || *pos != pathIdx) {
            list.insert(pos, pathIdx);
        }
    }
}

void SearchEngine::markPageDirty(uint32_t recordIndex) {
    uint32_t page = recordIndex / kRecordsPerPage;
    if (page < dirtyPages_.size()) {
        dirtyPages_[page] = true;
    }
}

std::vector<uint32_t> SearchEngine::getDirtyPageNumbers() const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    for (size_t i = 0; i < dirtyPages_.size(); i++) {
        if (dirtyPages_[i]) result.push_back(static_cast<uint32_t>(i));
    }
    return result;
}

void SearchEngine::clearDirtyPages() {
    std::unique_lock lock(mutex_);
    std::fill(dirtyPages_.begin(), dirtyPages_.end(), false);
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
                lowerNames_[i] = me::toLower(records_[i].name);
            }
        });
    }

    for (auto& th : threads) th.join();

    // Path deduplication: intern paths into PathTable, then clear record.path strings
    pathTable_.clear();
    pathIndices_.resize(records_.size());
    for (size_t i = 0; i < records_.size(); i++) {
        pathIndices_[i] = pathTable_.intern(records_[i].path);
        records_[i].path.clear();
    }

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
                    loweredPaths[i] = me::toLower(makeFullPath(pathTable_.resolve(pathIndices_[i]), records_[i].name));
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
    // After the loop above, pathIndex_ maps each path to the LAST record index.
    // Earlier records at the same path are orphans — tombstone them.
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
            // Orphaned duplicate — tombstone it
            records_[i].type = 0;
            records_[i].name.clear();
            records_[i].size = 0;
            records_[i].modTime = 0;
            lowerNames_[i].clear();
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

    auto queryStart = std::chrono::steady_clock::now();
    std::string lowerKey = me::toLower(keyword);
    bool useGlob = isGlobPattern(lowerKey);
    bool hasSlash = lowerKey.find('/') != std::string::npos;

    // C-1 fix: Hold shared_lock for the entire query to prevent use-after-free.
    // compactRecords() replaces records_/lowerNames_ via move-assign under unique_lock,
    // which would free the old storage. shared_lock prevents compaction during query.
    // dispatch_apply under shared_lock is safe — multiple readers can hold it concurrently.
    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (records_.empty()) return {};
    size_t totalSize = records_.size();

    // Timing instrumentation: default all phase timestamps to afterLock
    auto beforeTrigram = afterLock, afterTrigram = afterLock;
    auto afterPhase1 = afterLock;
    auto beforePhase2 = afterLock, afterPhase2 = afterLock;
    size_t phase1Results = 0;

    std::vector<uint32_t> trigramCandidates;
    bool useTrigramIndex = false;
    bool useSlashSplit = false;

    // --- Trigram-accelerated path for non-glob queries with keyword >= 3 chars ---
    useTrigramIndex = !useGlob && lowerKey.size() >= 3 && !nameTrigramIndex_.empty();

    if (useTrigramIndex) {
        beforeTrigram = std::chrono::steady_clock::now();
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

                // H-1 fix: reserve + assign avoids reallocation during copy
                trigramCandidates.reserve(postingLists[0]->size());
                trigramCandidates.assign(postingLists[0]->begin(), postingLists[0]->end());

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
        afterTrigram = std::chrono::steady_clock::now();
    }

    // Collect results: (index, priority, pathLen) so sorting doesn't need records_ access.
    // Priority: 0=name exact match, 1=name starts with, 2=name contains, 3=path-only match
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };
    std::vector<Match> merged;

    if (useTrigramIndex) {
        // Phase 1: Check trigram candidates for name matches
        // Single-threaded with prefetch
        for (size_t ci = 0; ci < trigramCandidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            uint32_t idx = trigramCandidates[ci];
            // Software prefetch: hide random-access memory latency
            if (ci + 4 < trigramCandidates.size()) {
                __builtin_prefetch(&lowerNames_[trigramCandidates[ci + 4]], 0, 1);
            }
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
                uint32_t pLen = static_cast<uint32_t>(pathTable_.resolve(pathIndices_[idx]).size() + 1 + records_[idx].name.size());
                merged.push_back({idx, priority, pLen});
            }
        }

        afterPhase1 = std::chrono::steady_clock::now();
        phase1Results = merged.size();

        // Phase 2: Path-only matches (skip if maxResults already satisfied)
        if (maxResults > 0 && merged.size() >= maxResults) {
            beforePhase2 = afterPhase2 = afterPhase1;
        } else {
        beforePhase2 = std::chrono::steady_clock::now();

        // Determine which path strategy to use
        // Slash-split: for queries like "tests/test_query" or "/usr/local/bin"
        // pathTrigramIndex_ indexes full directory paths (including '/'), so absolute
        // paths can also use slash-split — their trigrams exist in the index.
        useSlashSplit = !pathTrigramIndex_.empty() && hasSlash;
        bool usePathTrigram = !pathTrigramIndex_.empty()
                              && !useSlashSplit
                              && !hasSlash
                              && lowerKey.size() >= 3;

        if (useSlashSplit) {
            // Split keyword at last '/' into pathPart and namePart
            size_t lastSlash = lowerKey.rfind('/');
            std::string pathPart = lowerKey.substr(0, lastSlash);   // e.g. "tests"
            std::string namePart = lowerKey.substr(lastSlash + 1);  // e.g. "test_query_perf"

            // When pathPart is empty (e.g., "/etc" → pathPart="", namePart="etc"),
            // use the full lowerKey as pathPart for pathTrigramIndex_ lookup, since
            // the query is really a path pattern like "/etc" and pathTrigramIndex_
            // indexes full directory paths including '/'.
            if (pathPart.empty() && lowerKey.size() >= 3) {
                pathPart = lowerKey;
            }

            bool pathPartUsable = pathPart.size() >= 3;
            bool namePartUsable = namePart.size() >= 3;

            // If both parts are too short, fall through to linear scan
            if (!pathPartUsable && !namePartUsable) {
                useSlashSplit = false;
            } else {
                // Step 1: Get candidate pathIdxs from pathTrigramIndex_ (if pathPart >= 3)
                std::vector<uint32_t> candidatePathIdxs;
                bool pathFound = true;
                if (pathPartUsable) {
                    auto pathTrigrams = ContentIndex::extractTrigrams(pathPart);
                    std::unordered_set<Trigram> uniquePT(pathTrigrams.begin(), pathTrigrams.end());
                    std::vector<const std::vector<uint32_t>*> pathPostingLists;
                    for (Trigram t : uniquePT) {
                        auto it = pathTrigramIndex_.find(t);
                        if (it == pathTrigramIndex_.end()) { pathFound = false; break; }
                        pathPostingLists.push_back(&it->second);
                    }
                    if (pathFound && !pathPostingLists.empty()) {
                        std::sort(pathPostingLists.begin(), pathPostingLists.end(),
                            [](const auto* a, const auto* b) { return a->size() < b->size(); });
                        candidatePathIdxs.assign(pathPostingLists[0]->begin(), pathPostingLists[0]->end());
                        for (size_t li = 1; li < pathPostingLists.size() && !candidatePathIdxs.empty(); li++) {
                            const auto& other = *pathPostingLists[li];
                            std::vector<uint32_t> isect;
                            isect.reserve(std::min(candidatePathIdxs.size(), other.size()));
                            std::set_intersection(candidatePathIdxs.begin(), candidatePathIdxs.end(),
                                                  other.begin(), other.end(), std::back_inserter(isect));
                            candidatePathIdxs = std::move(isect);
                        }
                    }
                }

                // Step 2: Get candidate record indices from nameTrigramIndex_ (if namePart >= 3)
                std::vector<uint32_t> nameRecCandidates;
                bool nameFound = true;
                if (namePartUsable && !nameTrigramIndex_.empty()) {
                    auto nameTrigrams = ContentIndex::extractTrigrams(namePart);
                    std::unordered_set<Trigram> uniqueNT(nameTrigrams.begin(), nameTrigrams.end());
                    std::vector<const std::vector<uint32_t>*> namePostingLists;
                    for (Trigram t : uniqueNT) {
                        auto it = nameTrigramIndex_.find(t);
                        if (it == nameTrigramIndex_.end()) { nameFound = false; break; }
                        namePostingLists.push_back(&it->second);
                    }
                    if (nameFound && !namePostingLists.empty()) {
                        std::sort(namePostingLists.begin(), namePostingLists.end(),
                            [](const auto* a, const auto* b) { return a->size() < b->size(); });
                        nameRecCandidates.assign(namePostingLists[0]->begin(), namePostingLists[0]->end());
                        for (size_t li = 1; li < namePostingLists.size() && !nameRecCandidates.empty(); li++) {
                            const auto& other = *namePostingLists[li];
                            std::vector<uint32_t> isect;
                            isect.reserve(std::min(nameRecCandidates.size(), other.size()));
                            std::set_intersection(nameRecCandidates.begin(), nameRecCandidates.end(),
                                                  other.begin(), other.end(), std::back_inserter(isect));
                            nameRecCandidates = std::move(isect);
                        }
                    }
                }

                // Step 3: Combine candidates and verify full path match
                // Build dedup set from Phase 1
                std::vector<bool> isCandidate(totalSize, false);
                for (uint32_t idx : trigramCandidates) {
                    isCandidate[idx] = true;
                }

                if (pathPartUsable && pathFound && namePartUsable && nameFound) {
                    // Both indexes usable: expand pathIdxs to records, intersect with name candidates
                    size_t mergedBefore = merged.size();
                    std::unordered_set<uint32_t> nameSet(nameRecCandidates.begin(), nameRecCandidates.end());
                    for (uint32_t pi : candidatePathIdxs) {
                        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                        if (pi >= pathIdxToRecords_.size()) continue;
                        const auto& recIndices = pathIdxToRecords_[pi];
                        for (uint32_t idx : recIndices) {
                            if (records_[idx].type == 0) continue;
                            if (isCandidate[idx]) continue;
                            if (nameSet.find(idx) == nameSet.end()) continue;
                            // Verify full path substring match
                            const auto& rPath = pathTable_.resolve(pi);
                            std::string fullPath = me::toLower(makeFullPath(rPath, records_[idx].name));
                            if (fullPath.find(lowerKey) == std::string::npos) continue;
                            const auto& lowerName = lowerNames_[idx];
                            uint8_t priority = lowerName.find(lowerKey) != std::string::npos
                                ? (lowerName == lowerKey ? 0 : (lowerName.compare(0, lowerKey.size(), lowerKey) == 0 ? 1 : 2))
                                : 3;
                            uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records_[idx].name.size());
                            merged.push_back({idx, priority, pLen});
                        }
                    }
                    // Always supplement with path-only matching: the joint intersection
                    // only finds records whose filename contains namePart, but for queries
                    // like "/usr/local/bin" the user wants all files *under* that path,
                    // not just files with "bin" in their name.
                    if (!candidatePathIdxs.empty()) {
                        // Mark joint results as candidates so path-only doesn't duplicate them
                        for (size_t mi = mergedBefore; mi < merged.size(); mi++) {
                            isCandidate[merged[mi].idx] = true;
                        }
                        for (uint32_t pi : candidatePathIdxs) {
                            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                            if (pi >= pathIdxToRecords_.size()) continue;
                            const auto& rPath = pathTable_.resolve(pi);
                            std::string lowerPath = me::toLower(rPath);
                            if (lowerPath.find(pathPart) == std::string::npos) continue;
                            const auto& recIndices = pathIdxToRecords_[pi];
                            for (uint32_t idx : recIndices) {
                                if (records_[idx].type == 0) continue;
                                if (isCandidate[idx]) continue;
                                std::string fullPath = me::toLower(makeFullPath(rPath, records_[idx].name));
                                if (fullPath.find(lowerKey) == std::string::npos) continue;
                                const auto& lowerName = lowerNames_[idx];
                                uint8_t priority = lowerName.find(lowerKey) != std::string::npos
                                    ? (lowerName == lowerKey ? 0 : (lowerName.compare(0, lowerKey.size(), lowerKey) == 0 ? 1 : 2))
                                    : 3;
                                uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records_[idx].name.size());
                                merged.push_back({idx, priority, pLen});
                            }
                        }
                    }
                } else if (pathPartUsable && pathFound && !candidatePathIdxs.empty()) {
                    // Only path index usable: expand paths, verify name + full path
                    for (uint32_t pi : candidatePathIdxs) {
                        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                        if (pi >= pathIdxToRecords_.size()) continue;
                        const auto& rPath = pathTable_.resolve(pi);
                        std::string lowerPath = me::toLower(rPath);
                        if (lowerPath.find(pathPart) == std::string::npos) continue;
                        const auto& recIndices = pathIdxToRecords_[pi];
                        for (uint32_t idx : recIndices) {
                            if (records_[idx].type == 0) continue;
                            if (isCandidate[idx]) continue;
                            std::string fullPath = me::toLower(makeFullPath(rPath, records_[idx].name));
                            if (fullPath.find(lowerKey) == std::string::npos) continue;
                            const auto& lowerName = lowerNames_[idx];
                            uint8_t priority = lowerName.find(lowerKey) != std::string::npos
                                ? (lowerName == lowerKey ? 0 : (lowerName.compare(0, lowerKey.size(), lowerKey) == 0 ? 1 : 2))
                                : 3;
                            uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records_[idx].name.size());
                            merged.push_back({idx, priority, pLen});
                        }
                    }
                } else if (namePartUsable && nameFound && !nameRecCandidates.empty()) {
                    // Only name index usable: check name candidates, verify path + full path
                    for (uint32_t idx : nameRecCandidates) {
                        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                        if (records_[idx].type == 0) continue;
                        if (isCandidate[idx]) continue;
                        const auto& rPath = pathTable_.resolve(pathIndices_[idx]);
                        std::string fullPath = me::toLower(makeFullPath(rPath, records_[idx].name));
                        if (fullPath.find(lowerKey) == std::string::npos) continue;
                        const auto& lowerName = lowerNames_[idx];
                        uint8_t priority = lowerName.find(lowerKey) != std::string::npos
                            ? (lowerName == lowerKey ? 0 : (lowerName.compare(0, lowerKey.size(), lowerKey) == 0 ? 1 : 2))
                            : 3;
                        uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records_[idx].name.size());
                        merged.push_back({idx, priority, pLen});
                    }
                }
                // else: both parts too short or trigrams missing → 0 results from split path
                // (useSlashSplit was set to false above for both < 3, so won't reach here)
            }
        }

        if (useSlashSplit) {
            // Already handled above — skip to end of Phase 2
        } else if (usePathTrigram) {
            // Extract trigrams from keyword and intersect pathTrigramIndex_ posting lists
            auto keyTrigrams = ContentIndex::extractTrigrams(lowerKey);
            std::unordered_set<Trigram> uniquePathTrigrams(keyTrigrams.begin(), keyTrigrams.end());

            std::vector<uint32_t> candidatePathIdxs;
            bool pathAllFound = true;

            if (!uniquePathTrigrams.empty()) {
                std::vector<const std::vector<uint32_t>*> pathPostingLists;
                for (Trigram t : uniquePathTrigrams) {
                    auto it = pathTrigramIndex_.find(t);
                    if (it == pathTrigramIndex_.end()) {
                        pathAllFound = false;
                        break;
                    }
                    pathPostingLists.push_back(&it->second);
                }

                if (pathAllFound && !pathPostingLists.empty()) {
                    std::sort(pathPostingLists.begin(), pathPostingLists.end(),
                        [](const auto* a, const auto* b) { return a->size() < b->size(); });

                    candidatePathIdxs.assign(pathPostingLists[0]->begin(), pathPostingLists[0]->end());
                    for (size_t li = 1; li < pathPostingLists.size() && !candidatePathIdxs.empty(); li++) {
                        const auto& other = *pathPostingLists[li];
                        std::vector<uint32_t> intersection;
                        intersection.reserve(std::min(candidatePathIdxs.size(), other.size()));
                        std::set_intersection(candidatePathIdxs.begin(), candidatePathIdxs.end(),
                                              other.begin(), other.end(),
                                              std::back_inserter(intersection));
                        candidatePathIdxs = std::move(intersection);
                    }
                }
            }

            // Expand pathIdx -> record indices, excluding Phase 1 trigramCandidates
            if (pathAllFound && !candidatePathIdxs.empty()) {
                // Build O(1) bitset from trigramCandidates for dedup
                std::vector<bool> isCandidate(totalSize, false);
                for (uint32_t idx : trigramCandidates) {
                    isCandidate[idx] = true;
                }

                for (uint32_t pi : candidatePathIdxs) {
                    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                    if (pi >= pathIdxToRecords_.size()) continue;

                    // Verify this path actually contains the keyword (false positive filter)
                    const auto& rPath = pathTable_.resolve(pi);
                    std::string lowerPath = me::toLower(rPath);
                    if (lowerPath.find(lowerKey) == std::string::npos) continue;

                    const auto& recIndices = pathIdxToRecords_[pi];
                    for (uint32_t idx : recIndices) {
                        if (records_[idx].type == 0) continue;
                        if (isCandidate[idx]) continue; // already matched in Phase 1
                        // Already confirmed path matches; check if name also matches for priority
                        const auto& lowerName = lowerNames_[idx];
                        uint8_t priority;
                        if (lowerName.find(lowerKey) != std::string::npos) {
                            if (lowerName == lowerKey) priority = 0;
                            else if (lowerName.size() >= lowerKey.size() &&
                                     lowerName.compare(0, lowerKey.size(), lowerKey) == 0) priority = 1;
                            else priority = 2;
                        } else {
                            priority = 3; // path-only match
                        }
                        uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records_[idx].name.size());
                        merged.push_back({idx, priority, pLen});
                    }
                }
            }
            // If !pathAllFound (some trigram missing), zero path matches — skip Phase 2 entirely

        } else {
            // Fallback: parallel linear scan for path matches (glob, short keywords, or keywords with '/')
            unsigned numThreads = std::thread::hardware_concurrency();
            if (numThreads < 1) numThreads = 1;
            if (numThreads > 32) numThreads = 32;

            size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

            std::vector<std::vector<Match>> threadResults(numThreads);
            auto* threadResultsPtr = &threadResults;

            const auto& records = records_;
            const auto& lowerNames = lowerNames_;
            const auto& pTable = pathTable_;
            const auto& pIndices = pathIndices_;

            std::vector<bool> isCandidate(totalSize, false);
            for (uint32_t idx : trigramCandidates) {
                isCandidate[idx] = true;
            }

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
                    if (isCandidate[i]) continue;

                    const auto& lowerName = lowerNames[i];
                    if (lowerName.find(lowerKey) != std::string::npos) {
                        uint8_t priority;
                        if (lowerName == lowerKey) priority = 0;
                        else if (lowerName.size() >= lowerKey.size() &&
                                 lowerName.compare(0, lowerKey.size(), lowerKey) == 0) priority = 1;
                        else priority = 2;
                        const auto& rPath = pTable.resolve(pIndices[i]);
                        uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records[i].name.size());
                        local.push_back({static_cast<uint32_t>(i), priority, pLen});
                    } else {
                        const auto& rPath = pTable.resolve(pIndices[i]);
                        std::string lowerPath = me::toLower(makeFullPath(rPath, records[i].name));
                        if (lowerPath.find(lowerKey) != std::string::npos) {
                            uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records[i].name.size());
                            local.push_back({static_cast<uint32_t>(i), uint8_t(3), pLen});
                        }
                    }
                }
            });

            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

            for (auto& v : threadResults) {
                merged.insert(merged.end(), v.begin(), v.end());
            }
        }

        afterPhase2 = std::chrono::steady_clock::now();
        } // end Phase 2 maxResults check
    } else {
        // Original linear scan path (for glob patterns or short keywords)
        beforePhase2 = std::chrono::steady_clock::now();
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

        std::vector<std::vector<Match>> threadResults(numThreads);

        auto* threadResultsPtr = &threadResults;
        // C-1 fix: Access members directly — shared_lock held at function scope
        const auto& records = records_;
        const auto& lowerNames = lowerNames_;
        const auto& pTable = pathTable_;
        const auto& pIndices = pathIndices_;

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
                const auto& rPath = pTable.resolve(pIndices[i]);

                bool nameMatch = useGlob ? globMatch(lowerKey, lowerName)
                                         : (lowerName.find(lowerKey) != std::string::npos);
                bool pathMatch = false;
                if (!nameMatch) {
                    // Compute lowercase full path on-the-fly (avoids storing lowerPaths_ vector)
                    std::string lowerPath = me::toLower(makeFullPath(rPath, records[i].name));
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
                    uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + records[i].name.size());
                    local.push_back({static_cast<uint32_t>(i), priority, pLen});
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
        }
        afterPhase2 = std::chrono::steady_clock::now();
    }

    // Final cancellation check before sorting
    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

    // C-1 fix: Release shared_lock before sorting — Match structs contain only
    // local data (idx, priority, pathLen), no references into records_/lowerNames_.
    auto beforeUnlock = std::chrono::steady_clock::now();
    lock.unlock();

    // Sort by priority, then by full path length (shorter = shallower = better)
    auto beforeSort = std::chrono::steady_clock::now();
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
    auto afterSort = std::chrono::steady_clock::now();

    std::vector<uint32_t> result;
    result.reserve(resultCount);
    for (size_t i = 0; i < resultCount; i++) {
        result.push_back(merged[i].idx);
    }

    auto elapsed = std::chrono::steady_clock::now() - queryStart;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (ms > 100) {
        auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterLock - beforeLock).count();
        auto lockHeldMs = std::chrono::duration_cast<std::chrono::milliseconds>(beforeUnlock - afterLock).count();
        auto sortMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterSort - beforeSort).count();

        if (useTrigramIndex) {
            auto trigramMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterTrigram - beforeTrigram).count();
            auto phase1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(afterPhase1 - afterTrigram).count();
            auto phase2Ms = std::chrono::duration_cast<std::chrono::milliseconds>(afterPhase2 - beforePhase2).count();
            LOG_INFO("SearchEngine", "Query \"" << keyword << "\" total=" << ms
                << "ms lock_wait=" << lockWaitMs << "ms trigram=" << trigramMs
                << "ms phase1=" << phase1Ms << "ms phase2=" << phase2Ms
                << "ms lock_held=" << lockHeldMs << "ms sort=" << sortMs
                << "ms | path=" << (useSlashSplit ? "trigram-split" : "trigram") << " candidates=" << trigramCandidates.size()
                << " phase1=" << phase1Results
                << " phase2=" << (merged.size() - phase1Results)
                << " results=" << result.size() << " records=" << totalSize);
        } else {
            auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterPhase2 - beforePhase2).count();
            LOG_INFO("SearchEngine", "Query \"" << keyword << "\" total=" << ms
                << "ms lock_wait=" << lockWaitMs << "ms scan=" << scanMs
                << "ms lock_held=" << lockHeldMs << "ms sort=" << sortMs
                << "ms | path=linear results=" << result.size() << " records=" << totalSize);
        }
    }

    return result;
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= records_.size()) return {};
    FileRecord r = records_[index];
    // Reconstruct path from PathTable (record.path was cleared after interning)
    if (r.type != 0 && index < pathIndices_.size()) {
        r.path = pathTable_.resolve(pathIndices_[index]);
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
        lowerNames_[oldIdx].clear();
        pathIndex_.erase(existIt);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(oldIdx, oldModTime);
    }

    // Intern path before moving record
    uint32_t pIdx = pathTable_.intern(record.path);
    // If this is a new path, update pathTrigramIndex_
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    record.path.clear();
    record.path.shrink_to_fit();

    records_.push_back(std::move(record));
    lowerNames_.push_back(lower);
    pathIndices_.push_back(pIdx);
    pathIndex_[lowerFull] = idx;

    // Update trigram index
    addTrigramsForRecord(idx, lower);
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

bool SearchEngine::removeByPath(const std::string& fullPath) {
    std::unique_lock lock(mutex_);

    auto it = pathIndex_.find(me::toLower(fullPath));
    if (it == pathIndex_.end()) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    uint32_t idx = it->second;
    time_t oldModTime = records_[idx].modTime;
    // Clean up trigram index (must happen before clearing lowerNames_)
    removeTrigramsForRecord(idx);
    removePathTrigramsForRecord(idx);
    records_[idx].type = 0;
    markPageDirty(idx);
    records_[idx].name.clear();
    records_[idx].size = 0;
    records_[idx].modTime = 0;
    lowerNames_[idx].clear();
    pathIndex_.erase(it);

    liveCount_.fetch_sub(1, std::memory_order_relaxed);
    removeFromRecentCache(idx, oldModTime);

    return true;
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
                std::string fullPath = makeFullPath(pathTable_.resolve(pathIndices_[idx]), records_[idx].name);
                wal_->append(WALOp::Remove, fullPath);
            }

            time_t oldModTime = records_[idx].modTime;
            // Clean up trigram index (must happen before clearing lowerNames_)
            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            records_[idx].type = 0;
            markPageDirty(idx);
            records_[idx].name.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            lowerNames_[idx].clear();
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
                std::string fullPath = makeFullPath(pathTable_.resolve(pathIndices_[idx]), records_[idx].name);
                wal_->append(WALOp::Remove, fullPath);
            }

            removeTrigramsForRecord(idx);
            removePathTrigramsForRecord(idx);
            records_[idx].type = 0;
            markPageDirty(idx);
            records_[idx].name.clear();
            records_[idx].size = 0;
            records_[idx].modTime = 0;
            lowerNames_[idx].clear();
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

        uint32_t pIdx = pathTable_.intern(record.path);
        if (pIdx >= pathIdxToRecords_.size()) {
            ensurePathTrigramsForPathIdx(pIdx);
        }
        record.path.clear();
        record.path.shrink_to_fit();

        records_.push_back(std::move(record));
        lowerNames_.push_back(lower);
        pathIndices_.push_back(pIdx);
        pathIndex_[me::toLower(fullPath)] = newIdx;
        addTrigramsForRecord(newIdx, lower);
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

void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    std::unique_lock lock(mutex_);

    if (wal_) wal_->append(WALOp::Update, fullPath, updated);

    // Remove old record if exists (case-insensitive lookup)
    auto it = pathIndex_.find(me::toLower(fullPath));
    if (it != pathIndex_.end()) {
        uint32_t idx = it->second;
        time_t oldModTime = records_[idx].modTime;
        // Clean up trigram index (must happen before clearing lowerNames_)
        removeTrigramsForRecord(idx);
        removePathTrigramsForRecord(idx);
        records_[idx].type = 0;
        markPageDirty(idx);
        records_[idx].name.clear();
        records_[idx].size = 0;
        records_[idx].modTime = 0;
        lowerNames_[idx].clear();
        pathIndex_.erase(it);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(idx, oldModTime);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(records_.size());
    std::string newFullPath = makeFullPath(updated.path, updated.name);
    std::string lower = me::toLower(updated.name);

    // Intern path before moving record
    uint32_t pIdx = pathTable_.intern(updated.path);
    if (pIdx >= pathIdxToRecords_.size()) {
        ensurePathTrigramsForPathIdx(pIdx);
    }
    updated.path.clear();
    updated.path.shrink_to_fit();

    records_.push_back(std::move(updated));
    lowerNames_.push_back(lower);
    pathIndices_.push_back(pIdx);
    pathIndex_[me::toLower(newFullPath)] = newIdx;
    addTrigramsForRecord(newIdx, lower);
    addPathTrigramsForRecord(newIdx);
    if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
    }
    markPageDirty(newIdx);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(newIdx, records_[newIdx].modTime);
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    // ── Phase 1: Snapshot under shared_lock ──
    // Queries continue unblocked during snapshot copy.
    std::vector<FileRecord> snapRecords;
    std::vector<std::string> snapLowerNames;
    std::vector<uint32_t> snapPathIndices;
    PathTable snapPathTable;
    std::unordered_map<std::string, uint32_t> snapPathIndex;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        uint32_t live = liveCount_.load(std::memory_order_relaxed);
        if (live == records_.size()) return {}; // nothing to compact
        snapRecords = records_;
        snapLowerNames = lowerNames_;
        snapPathIndices = pathIndices_;
        snapPathTable = pathTable_;
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
    std::vector<std::string> cdLowerNames;
    cdLowerNames.reserve(snapSize);
    std::vector<uint32_t> cdPathIndices;
    cdPathIndices.reserve(snapSize);
    PathTable cdPathTable;
    std::unordered_map<std::string, uint32_t> cdPathIndex;
    cdPathIndex.reserve(snapSize);

    for (size_t i = 0; i < snapSize; i++) {
        if (snapRecords[i].type == 0) continue;
        // Skip orphaned duplicates: live records not referenced by pathIndex_
        const std::string& origPath = snapPathTable.resolve(snapPathIndices[i]);
        std::string fullPathLower = me::toLower(makeFullPath(origPath, snapRecords[i].name));
        auto pathIt = snapPathIndex.find(fullPathLower);
        if (pathIt == snapPathIndex.end() || pathIt->second != static_cast<uint32_t>(i)) continue;
        uint32_t newIdx = static_cast<uint32_t>(cdRecords.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        uint32_t newPIdx = cdPathTable.intern(origPath);
        cdPathIndex[fullPathLower] = newIdx;
        cdLowerNames.push_back(std::move(snapLowerNames[i]));
        cdPathIndices.push_back(newPIdx);
        cdRecords.push_back(std::move(snapRecords[i]));
    }
    uint32_t cdLiveCount = static_cast<uint32_t>(cdRecords.size());

    // Build trigram index, path trigram index, and recent cache outside any lock
    auto cdTrigramIndex = buildTrigramIndexFromData(cdRecords, cdLowerNames);
    auto cdPathTrigramIndex = buildPathTrigramIndexFromData(cdPathTable);
    auto cdPathIdxToRecords = buildPathIdxToRecordsFromData(cdRecords, cdPathIndices, cdPathTable.size());
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
        auto oldLowerNames = std::move(lowerNames_);
        auto oldPathIndices = std::move(pathIndices_);
        auto oldPathTable = std::move(pathTable_);
        auto oldPathIndex = std::move(pathIndex_);

        // Install compacted data
        records_ = std::move(cdRecords);
        lowerNames_ = std::move(cdLowerNames);
        pathIndices_ = std::move(cdPathIndices);
        pathTable_ = std::move(cdPathTable);
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
            const std::string& origPath = oldPathTable.resolve(oldPathIndices[i]);
            uint32_t pIdx = pathTable_.intern(origPath);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            std::string fullPath = me::toLower(makeFullPath(origPath, oldRecords[i].name));
            std::string lower = std::move(oldLowerNames[i]);
            pathIndex_[fullPath] = newIdx;
            pathIndices_.push_back(pIdx);
            lowerNames_.push_back(lower);
            addTrigramsForRecord(newIdx, lower);
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
                lowerNames_[newIdx].clear();
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
        r.path = pathTable_.resolve(pathIndices_[i]);
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
                lowerNames_[oldIdx].clear();
                pathIndex_.erase(existIt);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }

            uint32_t idx = static_cast<uint32_t>(records_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = pathTable_.intern(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            records_.push_back(std::move(e.record));
            lowerNames_.push_back(lower);
            pathIndices_.push_back(pIdx);
            pathIndex_[lowerFull] = idx;
            addTrigramsForRecord(idx, lower);
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
            lowerNames_[idx].clear();
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
                lowerNames_[oldIdx].clear();
                pathIndex_.erase(it);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }
            // Add updated record
            uint32_t newIdx = static_cast<uint32_t>(records_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = pathTable_.intern(e.record.path);
            if (pIdx >= pathIdxToRecords_.size()) {
                ensurePathTrigramsForPathIdx(pIdx);
            }
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            records_.push_back(std::move(e.record));
            lowerNames_.push_back(lower);
            pathIndices_.push_back(pIdx);
            pathIndex_[lowerFull] = newIdx;
            addTrigramsForRecord(newIdx, lower);
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
