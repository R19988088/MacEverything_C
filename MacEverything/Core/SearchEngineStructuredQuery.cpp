#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>

// ---------------------------------------------------------------------------
// pathSegmentsMatch: check if dirPath satisfies path segment constraints
// ---------------------------------------------------------------------------
bool SearchEngine::pathSegmentsMatch(const std::string& dirPath,
                                     const std::vector<PathSegment>& segments) {
    if (segments.empty()) return true;

    // Split dirPath into components by '/'
    std::vector<std::string_view> components;
    std::string_view dp(dirPath);
    size_t start = 0;
    for (size_t i = 0; i <= dp.size(); i++) {
        if (i == dp.size() || dp[i] == '/') {
            if (i > start) {
                components.push_back(dp.substr(start, i - start));
            }
            start = i + 1;
        }
    }

    // Match segments right-to-left against path components
    int segIdx = static_cast<int>(segments.size()) - 1;
    int compIdx = static_cast<int>(components.size()) - 1;

    while (segIdx >= 0 && compIdx >= 0) {
        // Lowercase comparison: check if component contains segment text
        std::string lowerComp;
        lowerComp.reserve(components[compIdx].size());
        for (char c : components[compIdx]) {
            lowerComp += (c >= 'A' && c <= 'Z') ? (c + 32) : c;
        }

        if (lowerComp.find(segments[segIdx].text) != std::string::npos) {
            // Segment matched at this component position
            segIdx--;
            compIdx--;
        } else {
            // If the previous segment requires adjacency, this is a mismatch
            if (segIdx < static_cast<int>(segments.size()) - 1 &&
                segments[segIdx + 1].adjacentToNext == false) {
                // Non-adjacent: skip this component, keep trying
                compIdx--;
            } else if (segIdx == static_cast<int>(segments.size()) - 1) {
                // First (rightmost) segment to match — can skip components
                compIdx--;
            } else {
                // Adjacent required but component doesn't match — fail
                return false;
            }
        }
    }

    return segIdx < 0; // All segments matched
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
    // Linear scan fallback
    // ------------------------------------------------------------------
    for (size_t i = 0; i < totalSize; i++) {
        if ((i & 4095) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return;
        if (records_[i].type == 0) continue;

        const char* nameData = namePool_.data(i);
        uint16_t nameLen = namePool_.length(i);

        if (pq.mode == QueryMode::DIR_EXACT) {
            if (records_[i].type != 2) continue;
            if (nameLen != namePattern.size()) continue;
            if (std::memcmp(nameData, namePattern.data(), nameLen) != 0) continue;
        } else {
            if (!me::simdContains(nameData, nameLen, namePattern.data(), namePattern.size())) continue;
        }

        if (!pq.pathSegments.empty()) {
            std::string dirPath(lowerPathPool_.view(pathIndices_[i]));
            if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
        }

        uint8_t priority = namePriority(nameData, nameLen, namePattern.data(), namePattern.size());
        uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[i]) + 1 + nameLen);
        merged.push_back({static_cast<uint32_t>(i), priority, pLen});
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
            std::string dirPath(lowerPathPool_.view(pathIndices_[idx]));
            if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
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
            std::string dirPath(lowerPathPool_.view(pathIndices_[idx]));
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
