#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// pathSegmentsMatch: check if dirPath satisfies path segment constraints
// ---------------------------------------------------------------------------
bool SearchEngine::pathSegmentsMatch(std::string_view dirPath,
                                     const std::vector<PathSegment>& segments) {
    if (segments.empty()) return true;

    // Split dirPath into components by '/'
    // dirPath comes from lowerPathPool_ which is already lowercase — no lowering needed
    std::vector<std::string_view> components;
    size_t start = 0;
    for (size_t i = 0; i <= dirPath.size(); i++) {
        if (i == dirPath.size() || dirPath[i] == '/') {
            if (i > start) {
                components.push_back(dirPath.substr(start, i - start));
            }
            start = i + 1;
        }
    }

    // Match segments right-to-left against path components
    int segIdx = static_cast<int>(segments.size()) - 1;
    int compIdx = static_cast<int>(components.size()) - 1;

    while (segIdx >= 0 && compIdx >= 0) {
        // Input is already lowercase — direct string_view find
        if (components[compIdx].find(segments[segIdx].text) != std::string_view::npos) {
            segIdx--;
            compIdx--;
        } else {
            if (segIdx < static_cast<int>(segments.size()) - 1 &&
                segments[segIdx + 1].adjacentToNext == false) {
                compIdx--;
            } else if (segIdx == static_cast<int>(segments.size()) - 1) {
                compIdx--;
            } else {
                return false;
            }
        }
    }

    return segIdx < 0;
}

// ---------------------------------------------------------------------------
// estimateTrigramCost: cheap upper-bound for trigram candidate count
// ---------------------------------------------------------------------------
size_t SearchEngine::estimateTrigramCost(const std::string& keyword) const {
    if (keyword.size() < 3 || nameTrigramIndex_.empty()) return SIZE_MAX;
    auto trigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(trigrams.begin(), trigrams.end());
    if (unique.empty()) return SIZE_MAX;

    size_t minSize = SIZE_MAX;
    for (Trigram t : unique) {
        auto it = nameTrigramIndex_.find(t);
        if (it == nameTrigramIndex_.end()) return 0; // trigram absent → 0 candidates
        minSize = std::min(minSize, it->second.size());
    }
    return minSize;
}

// ---------------------------------------------------------------------------
// treeWalkDown: walk children from anchor dir toward namePattern
// ---------------------------------------------------------------------------
void SearchEngine::treeWalkDown(uint32_t dirIdx, const ParsedQuery& pq,
                                int fromSegIdx, int toSegIdx,
                                size_t totalSize, uint64_t myGen,
                                std::vector<Match>& merged) const {
    // Build the full lowered directory path for this dir record
    auto lpView = lowerPathPool_.view(pathIndices_[dirIdx]);
    std::string fullDirPath(lpView);
    if (!fullDirPath.empty() && fullDirPath.back() != '/') fullDirPath += '/';
    fullDirPath += std::string(namePool_.data(dirIdx), namePool_.length(dirIdx));

    // Look up children of this directory
    auto it = lowerPathLookup_.find(fullDirPath);
    if (it == lowerPathLookup_.end()) return;

    uint32_t childPathIdx = it->second;
    if (childPathIdx >= pathIdxToRecords_.size()) return;
    const auto& childRecords = pathIdxToRecords_[childPathIdx];

    if (fromSegIdx > toSegIdx) {
        // We've walked through all intermediate segments.
        // Now match children against namePattern.
        const auto& namePattern = pq.namePattern;
        for (uint32_t childIdx : childRecords) {
            if (records_[childIdx].type == 0) continue;
            const char* nd = namePool_.data(childIdx);
            uint16_t nl = namePool_.length(childIdx);

            if (pq.mode == QueryMode::DIR_EXACT) {
                if (records_[childIdx].type != 2) continue;
                if (nl != namePattern.size()) continue;
                if (std::memcmp(nd, namePattern.data(), nl) != 0) continue;
            } else {
                if (!me::simdContains(nd, nl, namePattern.data(), namePattern.size())) continue;
            }

            uint8_t priority = namePriority(nd, nl, namePattern.data(), namePattern.size());
            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[childIdx]) + 1 + nl);
            merged.push_back({childIdx, priority, pLen});
        }
        return;
    }

    // Walk through intermediate segments: find children whose name matches
    // pathSegments[fromSegIdx], then recurse down
    const auto& segText = pq.pathSegments[fromSegIdx].text;
    for (uint32_t childIdx : childRecords) {
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;
        if (records_[childIdx].type != 2) continue; // must be directory
        const char* nd = namePool_.data(childIdx);
        uint16_t nl = namePool_.length(childIdx);
        // Segment match: name must contain segment text
        if (!me::simdContains(nd, nl, segText.data(), segText.size())) continue;

        // Recurse to next level
        treeWalkDown(childIdx, pq, fromSegIdx + 1, toSegIdx, totalSize, myGen, merged);
    }
}

// ---------------------------------------------------------------------------
// queryStructured: SEGMENTS and DIR_EXACT modes (with anchor-selection)
// ---------------------------------------------------------------------------
void SearchEngine::queryStructured(const ParsedQuery& pq,
                                   size_t totalSize, uint64_t myGen,
                                   std::vector<Match>& merged) const {
    const auto& namePattern = pq.namePattern;
    if (namePattern.empty()) return;

    // ------------------------------------------------------------------
    // Step 1: Estimate trigram cost for each segment + namePattern
    // ------------------------------------------------------------------
    size_t numPathSegs = pq.pathSegments.size();
    size_t nameCost = estimateTrigramCost(namePattern);

    size_t bestIdx = numPathSegs; // index into [pathSegs..., namePattern]
    size_t bestCost = nameCost;
    for (size_t i = 0; i < numPathSegs; i++) {
        size_t c = estimateTrigramCost(pq.pathSegments[i].text);
        // cost==0 for a path segment means its trigrams aren't in nameTrigramIndex_,
        // but that doesn't mean zero results — it just can't be used as an anchor.
        if (c > 0 && c < bestCost) {
            bestCost = c;
            bestIdx = i;
        }
    }

    size_t trigramThreshold = totalSize / 67;

    // ------------------------------------------------------------------
    // Step 2: Try anchor-based strategies before falling back to linear
    // ------------------------------------------------------------------
    // Only nameCost==0 guarantees zero results (namePattern must match a file name).
    // Path segment cost==0 just means that segment text isn't indexed as a name.
    if (nameCost == 0) return;

    if (bestCost <= trigramThreshold) {
        if (bestIdx == numPathSegs) {
            // Anchor is namePattern — original trigram path
            if (queryStructuredNameAnchor(pq, totalSize, myGen, merged))
                return;
        } else {
            // Anchor is a path segment — tree-walk strategy
            if (queryStructuredPathAnchor(pq, bestIdx, totalSize, myGen, merged))
                return;
        }
    }

    // ------------------------------------------------------------------
    // Linear scan fallback — parallel with pathMatchCache
    // ------------------------------------------------------------------

    // Pre-compute path segment matching: O(~100K) unique paths once,
    // then O(1) lookup per record via pathMatchCache
    bool hasPathSegs = !pq.pathSegments.empty();
    uint32_t pathCount = lowerPathPool_.entryCount();
    std::vector<bool> pathMatchCache;
    if (hasPathSegs) {
        pathMatchCache.resize(pathCount, false);
        for (uint32_t pi = 0; pi < pathCount; pi++) {
            if (!lowerPathPool_.isLive(pi)) continue;
            std::string_view lpv(lowerPathPool_.data(pi), lowerPathPool_.length(pi));
            if (pathSegmentsMatch(lpv, pq.pathSegments))
                pathMatchCache[pi] = true;
        }
    }

    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;
    size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

    std::vector<std::vector<Match>> threadResults(numThreads);
    auto* threadResultsPtr = &threadResults;
    const auto& records = records_;
    const auto& namePool = namePool_;
    const auto& pathPool = pathPool_;
    const auto& pIndices = pathIndices_;
    const auto* pathMatchCachePtr = &pathMatchCache;
    const auto* genPtr = &queryGeneration_;
    uint64_t capturedGen = myGen;

    // Capture query parameters for block
    auto mode = pq.mode;
    const auto& npRef = namePattern;

    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_apply(numThreads, queue, ^(size_t t) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, totalSize);
        if (start >= end) return;

        auto& local = (*threadResultsPtr)[t];
        for (size_t i = start; i < end; i++) {
            if ((i & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
            if (records[i].type == 0) continue;

            const char* nameData = namePool.data(static_cast<uint32_t>(i));
            uint16_t nameLen = namePool.length(static_cast<uint32_t>(i));

            if (mode == QueryMode::DIR_EXACT) {
                if (records[i].type != 2) continue;
                if (nameLen != npRef.size()) continue;
                if (std::memcmp(nameData, npRef.data(), nameLen) != 0) continue;
            } else {
                if (!me::simdContains(nameData, nameLen, npRef.data(), npRef.size())) continue;
            }

            if (hasPathSegs && !(*pathMatchCachePtr)[pIndices[i]]) continue;

            uint8_t priority = namePriority(nameData, nameLen, npRef.data(), npRef.size());
            uint32_t pLen = static_cast<uint32_t>(pathPool.length(pIndices[i]) + 1 + nameLen);
            local.push_back({static_cast<uint32_t>(i), priority, pLen});
        }
    });

    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;

    for (auto& v : threadResults) {
        merged.insert(merged.end(), v.begin(), v.end());
    }
}

// ---------------------------------------------------------------------------
// queryStructuredNameAnchor: trigram on namePattern, verify path constraints
// Returns true if handled (even if 0 results), false to fall back to linear.
// ---------------------------------------------------------------------------
bool SearchEngine::queryStructuredNameAnchor(const ParsedQuery& pq,
                                             size_t /*totalSize*/, uint64_t myGen,
                                             std::vector<Match>& merged) const {
    const auto& namePattern = pq.namePattern;
    bool allFound = false;
    auto candidates = intersectPostingLists(nameTrigramIndex_, namePattern, allFound);
    if (!allFound) return false;

    for (size_t ci = 0; ci < candidates.size(); ci++) {
        if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
        uint32_t idx = candidates[ci];
        if (records_[idx].type == 0) continue;

        const char* nameData = namePool_.data(idx);
        uint16_t nameLen = namePool_.length(idx);

        if (pq.mode == QueryMode::DIR_EXACT) {
            if (records_[idx].type != 2) continue;
            if (nameLen != namePattern.size()) continue;
            if (std::memcmp(nameData, namePattern.data(), nameLen) != 0) continue;
        } else {
            if (!me::simdContains(nameData, nameLen, namePattern.data(), namePattern.size())) continue;
        }

        if (!pq.pathSegments.empty()) {
            if (!pathSegmentsMatch(lowerPathPool_.view(pathIndices_[idx]), pq.pathSegments)) continue;
        }

        uint8_t priority = namePriority(nameData, nameLen, namePattern.data(), namePattern.size());
        uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nameLen);
        merged.push_back({idx, priority, pLen});
    }
    return true;
}

// ---------------------------------------------------------------------------
// queryStructuredPathAnchor: trigram on a path segment, tree-walk to name
// Returns true if handled, false to fall back to linear.
// ---------------------------------------------------------------------------
bool SearchEngine::queryStructuredPathAnchor(const ParsedQuery& pq,
                                             size_t anchorIdx,
                                             size_t totalSize, uint64_t myGen,
                                             std::vector<Match>& merged) const {
    size_t numPathSegs = pq.pathSegments.size();

    // Check that all segments from anchor to namePattern are adjacent.
    // If any gap is non-adjacent, we can't tree-walk → fall back.
    if (!pq.pathSegments[anchorIdx].adjacentToNext) return false;
    for (size_t s = anchorIdx + 1; s < numPathSegs; s++) {
        if (!pq.pathSegments[s].adjacentToNext) return false;
    }

    const auto& anchorText = pq.pathSegments[anchorIdx].text;
    bool allFound = false;
    auto candidates = intersectPostingLists(nameTrigramIndex_, anchorText, allFound);
    if (!allFound) return false;

    int walkFrom = static_cast<int>(anchorIdx) + 1;
    int walkTo = static_cast<int>(numPathSegs) - 1;

    for (size_t ci = 0; ci < candidates.size(); ci++) {
        if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
        uint32_t idx = candidates[ci];
        if (records_[idx].type == 0) continue;
        if (records_[idx].type != 2) continue; // anchor must be a directory

        const char* nd = namePool_.data(idx);
        uint16_t nl = namePool_.length(idx);
        if (!me::simdContains(nd, nl, anchorText.data(), anchorText.size())) continue;

        // Verify ancestor segments [0..anchorIdx-1] against this record's path
        if (anchorIdx > 0) {
            std::string_view dirPath = lowerPathPool_.view(pathIndices_[idx]);
            std::vector<PathSegment> ancestorSegs(
                pq.pathSegments.begin(),
                pq.pathSegments.begin() + static_cast<int>(anchorIdx));
            if (!pathSegmentsMatch(dirPath, ancestorSegs)) continue;
        }

        // Tree-walk down through remaining path segments to namePattern
        treeWalkDown(idx, pq, walkFrom, walkTo, totalSize, myGen, merged);
    }
    return true;
}
