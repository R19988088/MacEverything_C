#include "SearchEngine.h"
#include "StringUtils.h"
#include "Logger.h"
#include "QueryTokenizer.h"
#include "QueryParser.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <dispatch/dispatch.h>

// Thread-local reusable bitmap for dedup in query().
// Avoids per-query allocation of vector<bool>(totalSize) which costs ~625KB memset for 5M records.
// Uses dirty-tracking: only indices that were set are cleared, so reset is O(candidates) not O(totalSize).
namespace {
struct ReusableBitmap {
    std::vector<bool> bits;
    std::vector<uint32_t> dirty;

    void prepare(size_t size) {
        if (bits.size() < size) bits.resize(size, false);
        // Clear only previously dirtied bits
        for (uint32_t idx : dirty) bits[idx] = false;
        dirty.clear();
    }

    void set(uint32_t idx) {
        bits[idx] = true;
        dirty.push_back(idx);
    }

    bool test(uint32_t idx) const { return bits[idx]; }

    void populateFrom(const std::vector<uint32_t>& indices) {
        for (uint32_t idx : indices) set(idx);
    }
};

ReusableBitmap& threadLocalBitmap() {
    thread_local ReusableBitmap bm;
    return bm;
}
} // namespace

// ---------------------------------------------------------------------------
// Match priority
// ---------------------------------------------------------------------------

uint8_t SearchEngine::namePriority(const char* nameData, uint16_t nameLen,
                                   const char* keyData, size_t keyLen) {
    if (nameLen == keyLen && memcmp(nameData, keyData, nameLen) == 0)
        return 0; // exact match
    if (nameLen >= keyLen && memcmp(nameData, keyData, keyLen) == 0)
        return 1; // starts with
    return 2; // contains
}

// ---------------------------------------------------------------------------
// Full path buffer construction
// ---------------------------------------------------------------------------

size_t SearchEngine::buildFullPathBuf(std::vector<char>& buf,
                                      const char* pathData, uint16_t pathLen,
                                      const char* nameData, uint16_t nameLen) {
    size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
    if (buf.size() < fullLen) buf.resize(fullLen * 2);
    memcpy(buf.data(), pathData, pathLen);
    buf[pathLen] = '/';
    memcpy(buf.data() + pathLen + 1, nameData, nameLen);
    return fullLen;
}

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

bool SearchEngine::isGlobPattern(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

namespace {

bool globMatchImpl(const std::string& pattern, const std::string& text) {
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

// Extract all literal (non-wildcard) segments from a glob pattern.
// Splits at '*' and '?' boundaries, returns segments in order.
std::vector<std::string> extractLiteralSegments(const std::string& pattern) {
    std::vector<std::string> segments;
    std::string current;
    for (char c : pattern) {
        if (c == '*' || c == '?') {
            if (!current.empty()) {
                segments.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        segments.push_back(std::move(current));
    }
    return segments;
}

struct CompiledGlob {
    enum Type { SUFFIX, PREFIX, CONTAINS, EXACT, GENERIC };
    Type type;
    std::string fixed;                    // literal part for fast match (or best segment for GENERIC)
    std::string original;                 // original pattern for GENERIC fallback
    std::vector<std::string> segments;    // all literal segments >= 3 chars (for multi-segment trigram)
};

CompiledGlob compileGlob(const std::string& pattern) {
    // *.ext → SUFFIX(".ext")
    if (pattern.size() >= 2 && pattern[0] == '*' &&
        pattern.find('*', 1) == std::string::npos &&
        pattern.find('?', 1) == std::string::npos) {
        return {CompiledGlob::SUFFIX, pattern.substr(1), pattern};
    }
    // prefix* → PREFIX("prefix")
    if (pattern.size() >= 2 && pattern.back() == '*' &&
        pattern.find('*') == pattern.size() - 1 &&
        pattern.find('?') == std::string::npos) {
        return {CompiledGlob::PREFIX, pattern.substr(0, pattern.size() - 1), pattern};
    }
    // *keyword* → CONTAINS("keyword")
    if (pattern.size() >= 3 && pattern.front() == '*' && pattern.back() == '*') {
        std::string mid = pattern.substr(1, pattern.size() - 2);
        if (mid.find('*') == std::string::npos &&
            mid.find('?') == std::string::npos) {
            return {CompiledGlob::CONTAINS, mid, pattern};
        }
    }
    // No wildcard → EXACT
    if (pattern.find('*') == std::string::npos &&
        pattern.find('?') == std::string::npos) {
        return {CompiledGlob::EXACT, pattern, pattern};
    }
    // GENERIC: extract literal segments for trigram pre-filtering
    auto allSegs = extractLiteralSegments(pattern);
    std::string bestSeg;
    std::vector<std::string> qualifiedSegs;
    for (const auto& seg : allSegs) {
        if (seg.size() >= 3) {
            qualifiedSegs.push_back(seg);
            if (seg.size() > bestSeg.size()) {
                bestSeg = seg;
            }
        }
    }
    return {CompiledGlob::GENERIC, bestSeg, pattern, std::move(qualifiedSegs)};
}

bool compiledGlobMatch(const CompiledGlob& cg, const char* text, size_t len) {
    switch (cg.type) {
    case CompiledGlob::SUFFIX:
        return len >= cg.fixed.size() &&
               memcmp(text + len - cg.fixed.size(),
                      cg.fixed.data(), cg.fixed.size()) == 0;
    case CompiledGlob::PREFIX:
        return len >= cg.fixed.size() &&
               memcmp(text, cg.fixed.data(), cg.fixed.size()) == 0;
    case CompiledGlob::CONTAINS:
        return me::simdContains(text, static_cast<uint16_t>(std::min(len, size_t(65535))),
                                cg.fixed.data(), cg.fixed.size());
    case CompiledGlob::EXACT:
        return len == cg.fixed.size() &&
               memcmp(text, cg.fixed.data(), len) == 0;
    case CompiledGlob::GENERIC: {
        std::string s(text, len);
        return globMatchImpl(cg.original, s);
    }
    }
    return false;
}

} // anonymous namespace

bool SearchEngine::globMatch(const std::string& pattern, const std::string& text) {
    return globMatchImpl(pattern, text);
}

// ---------------------------------------------------------------------------
// Strategy methods extracted from query() — called under shared_lock
// ---------------------------------------------------------------------------

bool SearchEngine::querySlashSplit(const std::string& lowerKey,
                                   size_t totalSize, uint64_t myGen,
                                   const std::vector<uint32_t>& trigramCandidates,
                                   std::vector<Match>& merged) const {
    // Reusable buffer for full-path construction — avoids per-record heap allocations
    std::vector<char> pathBuf;

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

    // If both parts are too short, signal caller to try another strategy
    if (!pathPartUsable && !namePartUsable) {
        return false;
    }

    // Step 1: Get candidate pathIdxs from pathTrigramIndex_ (if pathPart >= 3)
    std::vector<uint32_t> candidatePathIdxs;
    bool pathFound = true;
    if (pathPartUsable) {
        candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, pathPart, pathFound);
    }

    // Step 2: Get candidate record indices from nameTrigramIndex_ (if namePart >= 3)
    std::vector<uint32_t> nameRecCandidates;
    bool nameFound = true;
    if (namePartUsable && !nameTrigramIndex_.empty()) {
        nameRecCandidates = intersectPostingLists(nameTrigramIndex_, namePart, nameFound);
    }

    // Step 3: Combine candidates and verify full path match
    // Build dedup set from Phase 1
    auto& isCandidate = threadLocalBitmap();
    isCandidate.prepare(totalSize);
    isCandidate.populateFrom(trigramCandidates);

    if (pathPartUsable && pathFound && namePartUsable && nameFound) {
        // Both indexes usable: expand pathIdxs to records, intersect with name candidates
        size_t mergedBefore = merged.size();
        std::unordered_set<uint32_t> nameSet(nameRecCandidates.begin(), nameRecCandidates.end());
        for (uint32_t pi : candidatePathIdxs) {
            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
            if (pi >= pathIdxToRecords_.size()) continue;
            const auto& recIndices = pathIdxToRecords_[pi];
            for (uint32_t idx : recIndices) {
                if (records_[idx].type == 0) continue;
                if (isCandidate.test(idx)) continue;
                if (nameSet.find(idx) == nameSet.end()) continue;
                const char* pd = lowerPathPool_.data(pi);
                uint16_t pl = lowerPathPool_.length(pi);
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
                if (!me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size())) continue;
                uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                    ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
                uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
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
                isCandidate.set(merged[mi].idx);
            }
            for (uint32_t pi : candidatePathIdxs) {
                if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
                if (pi >= pathIdxToRecords_.size()) continue;
                const char* pd2 = lowerPathPool_.data(pi);
                uint16_t pl2 = lowerPathPool_.length(pi);
                // Pre-filter: check if pre-lowered path contains pathPart
                if (!me::simdContains(pd2, pl2, pathPart.data(), pathPart.size())) continue;
                const auto& recIndices = pathIdxToRecords_[pi];
                for (uint32_t idx : recIndices) {
                    if (records_[idx].type == 0) continue;
                    if (isCandidate.test(idx)) continue;
                    const char* nd = namePool_.data(idx);
                    uint16_t nl = namePool_.length(idx);
                    size_t fullLen = buildFullPathBuf(pathBuf, pd2, pl2, nd, nl);
                    if (!me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size())) continue;
                    uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                        ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
                    uint32_t pLen = static_cast<uint32_t>(pl2 + 1 + nl);
                    merged.push_back({idx, priority, pLen});
                }
            }
        }
    } else if (pathPartUsable && pathFound && !candidatePathIdxs.empty()) {
        // Only path index usable: expand paths, verify name + full path
        for (uint32_t pi : candidatePathIdxs) {
            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
            if (pi >= pathIdxToRecords_.size()) continue;
            const char* pd = lowerPathPool_.data(pi);
            uint16_t pl = lowerPathPool_.length(pi);
            // Pre-filter: check if pre-lowered path contains pathPart
            if (!me::simdContains(pd, pl, pathPart.data(), pathPart.size())) continue;
            const auto& recIndices = pathIdxToRecords_[pi];
            for (uint32_t idx : recIndices) {
                if (records_[idx].type == 0) continue;
                if (isCandidate.test(idx)) continue;
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
                if (!me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size())) continue;
                uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                    ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
                uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
                merged.push_back({idx, priority, pLen});
            }
        }
    } else if (namePartUsable && nameFound && !nameRecCandidates.empty()) {
        // Only name index usable: check name candidates, verify path + full path
        for (uint32_t idx : nameRecCandidates) {
            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return true;
            if (records_[idx].type == 0) continue;
            if (isCandidate.test(idx)) continue;
            uint32_t pi = pathIndices_[idx];
            const char* pd = lowerPathPool_.data(pi);
            uint16_t pl = lowerPathPool_.length(pi);
            const char* nd = namePool_.data(idx);
            uint16_t nl = namePool_.length(idx);
            size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
            if (!me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size())) continue;
            uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
            uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
            merged.push_back({idx, priority, pLen});
        }
    }
    // else: both parts too short or trigrams missing → 0 results from split path

    return true;
}

// ---------------------------------------------------------------------------
// queryDirList: DIR_LIST mode — find directory, return its children
// ---------------------------------------------------------------------------
void SearchEngine::queryDirList(const ParsedQuery& pq,
                                size_t totalSize, uint64_t myGen,
                                std::vector<Match>& merged) const {
    const auto& dirName = pq.namePattern;
    if (dirName.empty()) return;

    // Find directory records whose name exactly matches dirName
    std::vector<uint32_t> dirIndices;

    if (dirName.size() >= 3 && !nameTrigramIndex_.empty()) {
        bool allFound = false;
        auto candidates = intersectPostingLists(nameTrigramIndex_, dirName, allFound);
        if (allFound && candidates.size() <= totalSize / 67) {
            for (uint32_t idx : candidates) {
                if (records_[idx].type != 2) continue; // must be directory
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                if (nl != dirName.size()) continue;
                if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
                // Check path constraints
                if (!pq.pathSegments.empty()) {
                    std::string dirPath = lowerPathPool_.str(pathIndices_[idx]);
                    if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
                }
                dirIndices.push_back(idx);
            }
        } else {
            // Fallback: linear scan for directories
            for (size_t i = 0; i < totalSize; i++) {
                if (records_[i].type != 2) continue;
                const char* nd = namePool_.data(i);
                uint16_t nl = namePool_.length(i);
                if (nl != dirName.size()) continue;
                if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
                if (!pq.pathSegments.empty()) {
                    std::string dirPath = lowerPathPool_.str(pathIndices_[i]);
                    if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
                }
                dirIndices.push_back(static_cast<uint32_t>(i));
            }
        }
    } else {
        // Short name or no trigram index — linear scan
        for (size_t i = 0; i < totalSize; i++) {
            if (records_[i].type != 2) continue;
            const char* nd = namePool_.data(i);
            uint16_t nl = namePool_.length(i);
            if (nl != dirName.size()) continue;
            if (std::memcmp(nd, dirName.data(), nl) != 0) continue;
            if (!pq.pathSegments.empty()) {
                std::string dirPath = lowerPathPool_.str(pathIndices_[i]);
                if (!pathSegmentsMatch(dirPath, pq.pathSegments)) continue;
            }
            dirIndices.push_back(static_cast<uint32_t>(i));
        }
    }

    if (dirIndices.empty()) return;

    // For each matching directory, find its children via pathLookup_ + pathIdxToRecords_
    for (uint32_t dirIdx : dirIndices) {
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;

        // Build the full directory path: parentPath + "/" + dirName
        std::string parentPath = pathPool_.str(pathIndices_[dirIdx]);
        std::string fullDirPath = parentPath;
        if (!fullDirPath.empty() && fullDirPath.back() != '/') fullDirPath += '/';
        fullDirPath += std::string(namePool_.data(dirIdx), namePool_.length(dirIdx));

        // Look up this full path in pathLookup_ to find its pathPool index
        auto it = pathLookup_.find(fullDirPath);
        if (it == pathLookup_.end()) continue;

        uint32_t childPathIdx = it->second;
        if (childPathIdx >= pathIdxToRecords_.size()) continue;

        const auto& childRecords = pathIdxToRecords_[childPathIdx];
        for (uint32_t childIdx : childRecords) {
            if (records_[childIdx].type == 0) continue; // skip tombstones
            const char* nd = namePool_.data(childIdx);
            uint16_t nl = namePool_.length(childIdx);
            uint8_t priority = 2; // children are all "contains" priority
            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[childIdx]) + 1 + nl);
            merged.push_back({childIdx, priority, pLen});
        }
    }
}

void SearchEngine::queryPathTrigram(const std::string& lowerKey,
                                    size_t totalSize, uint64_t myGen,
                                    const std::vector<uint32_t>& trigramCandidates,
                                    std::vector<Match>& merged) const {
    bool pathAllFound = false;
    std::vector<uint32_t> candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, lowerKey, pathAllFound);

    // Expand pathIdx -> record indices, excluding Phase 1 trigramCandidates
    if (pathAllFound && !candidatePathIdxs.empty()) {
        // Build O(1) bitset from trigramCandidates for dedup
        auto& isCandidate = threadLocalBitmap();
        isCandidate.prepare(totalSize);
        isCandidate.populateFrom(trigramCandidates);

        for (uint32_t pi : candidatePathIdxs) {
            if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;
            if (pi >= pathIdxToRecords_.size()) continue;

            // Verify this path actually contains the keyword (false positive filter)
            std::string_view lowerPath(lowerPathPool_.data(pi), lowerPathPool_.length(pi));
            if (lowerPath.find(lowerKey) == std::string_view::npos) continue;

            const auto& recIndices = pathIdxToRecords_[pi];
            for (uint32_t idx : recIndices) {
                if (records_[idx].type == 0) continue;
                if (isCandidate.test(idx)) continue;
                const char* nd = namePool_.data(idx);
                uint16_t nl = namePool_.length(idx);
                uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                    ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
                uint32_t pLen = static_cast<uint32_t>(lowerPath.size() + 1 + nl);
                merged.push_back({idx, priority, pLen});
            }
        }
    }
    // If !pathAllFound (some trigram missing), zero path matches — skip Phase 2 entirely
}

void SearchEngine::queryLinearScanPath(const std::string& lowerKey,
                                       size_t totalSize, uint64_t myGen,
                                       const std::vector<uint32_t>& trigramCandidates,
                                       std::vector<Match>& merged) const {
    // Fallback: parallel linear scan for path matches (glob, short keywords, or keywords with '/')
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;

    size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

    std::vector<std::vector<Match>> threadResults(numThreads);
    auto* threadResultsPtr = &threadResults;

    const auto& records = records_;
    const auto& namePool = namePool_;
    const auto& lowerPathPool = lowerPathPool_;
    const auto& pIndices = pathIndices_;

    auto& isCandidate = threadLocalBitmap();
    isCandidate.prepare(totalSize);
    isCandidate.populateFrom(trigramCandidates);
    const auto* isCandidateBits = &isCandidate.bits;

    // Dedup path matching for fallback linear scan
    bool hasSlashFb = lowerKey.find('/') != std::string::npos;
    uint32_t pathCountFb = lowerPathPool.entryCount();
    std::vector<bool> pathMatchCacheFb(pathCountFb, false);
    for (uint32_t pi = 0; pi < pathCountFb; pi++) {
        if (!lowerPathPool.isLive(pi)) continue;
        if (me::simdContains(lowerPathPool.data(pi), lowerPathPool.length(pi),
                             lowerKey.data(), lowerKey.size()))
            pathMatchCacheFb[pi] = true;
    }
    const auto* pathMatchCacheFbPtr = &pathMatchCacheFb;

    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    const auto* genPtr = &queryGeneration_;
    uint64_t capturedGen = myGen;
    dispatch_apply(numThreads, queue, ^(size_t t) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, totalSize);
        if (start >= end) return;

        auto& local = (*threadResultsPtr)[t];
        // Thread-local reusable buffer for full paths.
        std::vector<char> pathBuf;
        for (size_t i = start; i < end; i++) {
            if ((i & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
            if (records[i].type == 0) continue;
            if ((*isCandidateBits)[i]) continue;

            const char* nd = namePool.data(static_cast<uint32_t>(i));
            uint16_t nl = namePool.length(static_cast<uint32_t>(i));
            if (me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())) {
                uint8_t priority = namePriority(nd, nl, lowerKey.data(), lowerKey.size());
                uint32_t pLen = static_cast<uint32_t>(lowerPathPool.length(pIndices[i]) + 1 + nl);
                local.push_back({static_cast<uint32_t>(i), priority, pLen});
            } else {
                // O(1) dedup path check: skip if path doesn't match and no slash boundary
                if (!(*pathMatchCacheFbPtr)[pIndices[i]] && !hasSlashFb) continue;
                // Build full path from pre-lowered path pool (no runtime lowering needed)
                const char* pd = lowerPathPool.data(pIndices[i]);
                uint16_t pl = lowerPathPool.length(pIndices[i]);
                size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
                if (me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size())) {
                    local.push_back({static_cast<uint32_t>(i), uint8_t(3), static_cast<uint32_t>(fullLen)});
                }
            }
        }
    });

    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;

    for (auto& v : threadResults) {
        merged.insert(merged.end(), v.begin(), v.end());
    }
}

void SearchEngine::queryLinearScan(const std::string& lowerKey,
                                   bool useGlob, size_t totalSize, uint64_t myGen,
                                   std::vector<Match>& merged) const {
    // Original linear scan path (for glob patterns or short keywords)
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 1;
    if (numThreads > 32) numThreads = 32;

    size_t chunkSize = (totalSize + numThreads - 1) / numThreads;

    std::vector<std::vector<Match>> threadResults(numThreads);

    auto* threadResultsPtr = &threadResults;
    const auto& records = records_;
    const auto& namePool = namePool_;
    const auto& lowerPathPool = lowerPathPool_;
    const auto& pIndices = pathIndices_;

    // Dedup path matching: pre-scan ~100K unique paths once, then O(1) lookup per record
    bool hasSlash = !useGlob && lowerKey.find('/') != std::string::npos;
    uint32_t pathCount = lowerPathPool.entryCount();
    std::vector<bool> pathMatchCache(pathCount, false);
    if (!useGlob) {
        for (uint32_t pi = 0; pi < pathCount; pi++) {
            if (!lowerPathPool.isLive(pi)) continue;
            if (me::simdContains(lowerPathPool.data(pi), lowerPathPool.length(pi),
                                 lowerKey.data(), lowerKey.size()))
                pathMatchCache[pi] = true;
        }
    }
    const auto* pathMatchCachePtr = &pathMatchCache;

    // Pre-compile glob pattern once before dispatch_apply
    CompiledGlob cg;
    if (useGlob) cg = compileGlob(lowerKey);
    const auto* cgPtr = useGlob ? &cg : nullptr;

    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    const auto* genPtr = &queryGeneration_;
    uint64_t capturedGen = myGen;
    dispatch_apply(numThreads, queue, ^(size_t t) {
        size_t start = t * chunkSize;
        size_t end = std::min(start + chunkSize, totalSize);
        if (start >= end) return;

        auto& local = (*threadResultsPtr)[t];
        std::vector<char> pathBuf;
        for (size_t i = start; i < end; i++) {
            if ((i & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;
            if (records[i].type == 0) continue;

            const char* nd = namePool.data(static_cast<uint32_t>(i));
            uint16_t nl = namePool.length(static_cast<uint32_t>(i));

            bool nameMatch;
            if (useGlob) {
                nameMatch = compiledGlobMatch(*cgPtr, nd, nl);
            } else {
                nameMatch = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size());
            }
            bool pathMatch = false;
            if (!nameMatch) {
                if (!useGlob && !(*pathMatchCachePtr)[pIndices[i]] && !hasSlash) continue;
                const char* pd = lowerPathPool.data(pIndices[i]);
                uint16_t pl = lowerPathPool.length(pIndices[i]);
                size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
                if (useGlob) {
                    pathMatch = compiledGlobMatch(*cgPtr, pathBuf.data(), fullLen);
                } else {
                    pathMatch = me::simdContains(pathBuf.data(), fullLen, lowerKey.data(), lowerKey.size());
                }
            }

            if (nameMatch || pathMatch) {
                uint8_t priority = nameMatch ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
                uint32_t pLen = static_cast<uint32_t>(lowerPathPool.length(pIndices[i]) + 1 + nl);
                local.push_back({static_cast<uint32_t>(i), priority, pLen});
            }
        }
    });

    // Check if superseded after dispatch_apply
    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;

    for (auto& v : threadResults) {
        merged.insert(merged.end(), v.begin(), v.end());
    }
}

// ---------------------------------------------------------------------------
// Main query() entry points
// ---------------------------------------------------------------------------

std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults,
                                          bool useTrigram) const {
    QueryTimingInfo unused;
    return query(keyword, maxResults, useTrigram, unused);
}

std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults,
                                          bool useTrigram, QueryTimingInfo& timing) const {
    // Increment generation so any in-flight query detects it has been superseded.
    // Must happen before the empty check so clearing the search box also cancels stale queries.
    uint64_t myGen = queryGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (keyword.empty()) return {};

    // Expand leading ~ to the user's home directory so that patterns like
    // ~/*/*.txt match absolute indexed paths (e.g. /Users/wujian/Downloads/f1.txt).
    std::string expandedKw = keyword;
    if (!expandedKw.empty() && expandedKw[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            if (expandedKw.size() == 1) {
                expandedKw = home;
            } else if (expandedKw[1] == '/') {
                expandedKw = std::string(home) + expandedKw.substr(1);
            }
        }
    }

    // Route to advanced query path if the input contains boolean operators,
    // grouping, quoted phrases, or known filter functions.
    if (QueryTokenizer::hasAdvancedSyntax(expandedKw)) {
        return queryAdvanced(expandedKw, maxResults, useTrigram, timing);
    }

    auto queryStart = std::chrono::steady_clock::now();
    std::string lowerKey = me::toLower(expandedKw);
    auto parsedQuery = parseQuery(expandedKw);
    bool isStructured = (parsedQuery.mode != QueryMode::PLAIN);
    bool useGlob = !isStructured && isGlobPattern(lowerKey);
    bool hasSlash = lowerKey.find('/') != std::string::npos;

    // C-1 fix: Hold shared_lock for the entire query to prevent use-after-free.
    // compactRecords() replaces records_/namePool_ via move-assign under unique_lock,
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
    bool useTrigramIndex = useTrigram && !useGlob && lowerKey.size() >= 3 && !nameTrigramIndex_.empty();
    bool useSlashSplit = false;

    if (useTrigramIndex) {
        beforeTrigram = std::chrono::steady_clock::now();
        bool nameAllFound = false;
        trigramCandidates = intersectPostingLists(nameTrigramIndex_, lowerKey, nameAllFound);
        if (!nameAllFound) {
            useTrigramIndex = false;
        } else if (trigramCandidates.size() > totalSize / 67) { // ~1.5%
            trigramCandidates.clear();
            useTrigramIndex = false;
        }
        afterTrigram = std::chrono::steady_clock::now();
        if (!useTrigramIndex) {
            afterPhase1 = afterTrigram;  // Trigram degraded: no Phase 1, zero out phase1Ms
        }
    }

    std::vector<Match> merged;

    if (isStructured) {
        // Node-centric structured query dispatch
        beforePhase2 = std::chrono::steady_clock::now();
        if (parsedQuery.mode == QueryMode::DIR_LIST) {
            queryDirList(parsedQuery, totalSize, myGen, merged);
        } else {
            queryStructured(parsedQuery, totalSize, myGen, merged);
        }
        afterPhase2 = std::chrono::steady_clock::now();
    } else if (useTrigramIndex) {
        // Phase 1: Check trigram candidates for name matches
        for (size_t ci = 0; ci < trigramCandidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            uint32_t idx = trigramCandidates[ci];
            if (records_[idx].type == 0) continue;
            const char* nameData = namePool_.data(idx);
            uint16_t nameLen = namePool_.length(idx);
            if (me::simdContains(nameData, nameLen, lowerKey.data(), lowerKey.size())) {
                uint8_t priority = namePriority(nameData, nameLen, lowerKey.data(), lowerKey.size());
                uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nameLen);
                merged.push_back({idx, priority, pLen});
            }
        }

        afterPhase1 = std::chrono::steady_clock::now();
        phase1Results = merged.size();

        // Phase 2: Path-based supplemental matches (skip if maxResults already satisfied)
        if (maxResults > 0 && merged.size() >= maxResults) {
            beforePhase2 = afterPhase2 = afterPhase1;
        } else {
            beforePhase2 = std::chrono::steady_clock::now();

            useSlashSplit = !pathTrigramIndex_.empty() && hasSlash;
            bool usePathTri = !pathTrigramIndex_.empty() && !useSlashSplit && !hasSlash && lowerKey.size() >= 3;

            if (useSlashSplit) {
                useSlashSplit = querySlashSplit(lowerKey, totalSize, myGen, trigramCandidates, merged);
            }

            if (useSlashSplit) {
                // Already handled by querySlashSplit
            } else if (usePathTri) {
                queryPathTrigram(lowerKey, totalSize, myGen, trigramCandidates, merged);
            } else {
                queryLinearScanPath(lowerKey, totalSize, myGen, trigramCandidates, merged);
            }

            afterPhase2 = std::chrono::steady_clock::now();
        }
    } else {
        // Non-trigram path — glob can still try trigram pre-filtering
        beforePhase2 = std::chrono::steady_clock::now();
        if (useGlob) {
            auto cg = compileGlob(lowerKey);
            bool globUsedTrigram = false;
            if (useTrigram && cg.fixed.size() >= 3 && !nameTrigramIndex_.empty()) {
                beforeTrigram = std::chrono::steady_clock::now();
                bool allFound = false;
                auto candidates = (cg.segments.size() > 1)
                    ? intersectPostingListsMulti(nameTrigramIndex_, cg.segments, allFound)
                    : intersectPostingLists(nameTrigramIndex_, cg.fixed, allFound);
                afterTrigram = std::chrono::steady_clock::now();
                afterPhase1 = afterTrigram;  // Glob-trigram: no Phase 1, zero out phase1Ms
                if (allFound && candidates.size() <= totalSize / 67) {
                    globUsedTrigram = true;
                    std::vector<char> pathBuf;
                    for (size_t ci = 0; ci < candidates.size(); ci++) {
                        if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                        uint32_t idx = candidates[ci];
                        if (records_[idx].type == 0) continue;
                        const char* nd = namePool_.data(idx);
                        uint16_t nl = namePool_.length(idx);
                        if (compiledGlobMatch(cg, nd, nl)) {
                            uint8_t priority = namePriority(nd, nl, lowerKey.data(), lowerKey.size());
                            uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nl);
                            merged.push_back({idx, priority, pLen});
                        } else {
                            const char* pd = lowerPathPool_.data(pathIndices_[idx]);
                            uint16_t pl = lowerPathPool_.length(pathIndices_[idx]);
                            size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
                            if (compiledGlobMatch(cg, pathBuf.data(), fullLen)) {
                                uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
                                merged.push_back({idx, 3, pLen});
                            }
                        }
                    }
                    trigramCandidates = std::move(candidates);
                    useTrigramIndex = true;  // for searchPath label
                }
            }
            if (!globUsedTrigram) {
                queryLinearScan(lowerKey, useGlob, totalSize, myGen, merged);
            }
        } else if (hasSlash && useTrigram && !pathTrigramIndex_.empty()) {
            // Slash query: try trigram-split path directly.
            // The name-trigram gate (L972-983) rejects slash queries because
            // cross-slash trigrams (e.g. "r/l") don't exist in nameTrigramIndex_.
            // Bypass that gate and call querySlashSplit directly.
            beforeTrigram = std::chrono::steady_clock::now();
            std::vector<uint32_t> emptyPhase1;
            useSlashSplit = querySlashSplit(lowerKey, totalSize, myGen, emptyPhase1, merged);
            afterTrigram = std::chrono::steady_clock::now();
            afterPhase1 = afterTrigram;  // No Phase 1 in this path; zero out phase1Ms
            if (useSlashSplit) {
                useTrigramIndex = true;  // for searchPath label
            } else {
                // Both parts < 3 chars, fall back to linear scan
                queryLinearScan(lowerKey, false, totalSize, myGen, merged);
            }
        } else {
            queryLinearScan(lowerKey, useGlob, totalSize, myGen, merged);
        }
        afterPhase2 = std::chrono::steady_clock::now();
    }

    // Final cancellation check before sorting
    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

    // C-1 fix: Release shared_lock before sorting — Match structs contain only
    // local data (idx, priority, pathLen), no references into records_/namePool_.
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

    // Populate timing info (always, using microsecond precision)
    auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
    timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
    timing.lockWaitMs = toMs(afterLock - beforeLock);
    timing.lockHeldMs = toMs(beforeUnlock - afterLock);
    timing.sortMs = toMs(afterSort - beforeSort);
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = isStructured ? 0.0 : toMs(afterPhase1 - afterTrigram);
    timing.phase2Ms = toMs(afterPhase2 - beforePhase2);
    timing.totalRecords = totalSize;
    timing.candidates = trigramCandidates.size();
    timing.nameMatches = phase1Results;
    timing.pathMatches = merged.size() > phase1Results ? merged.size() - phase1Results : 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex;
    if (isStructured) {
        timing.searchPath = (parsedQuery.mode == QueryMode::DIR_LIST) ? "dir-list" : "structured";
    } else {
        timing.searchPath = useTrigramIndex ? (useGlob ? "glob-trigram" : (useSlashSplit ? "trigram-split" : "trigram")) : "linear";
    }

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        if (useTrigramIndex) {
            LOG_INFO("SearchEngine", "Query \"" << keyword << "\" total=" << ms
                << "ms lock_wait=" << static_cast<long long>(timing.lockWaitMs) << "ms trigram=" << static_cast<long long>(timing.trigramMs)
                << "ms phase1=" << static_cast<long long>(timing.phase1Ms) << "ms phase2=" << static_cast<long long>(timing.phase2Ms)
                << "ms lock_held=" << static_cast<long long>(timing.lockHeldMs) << "ms sort=" << static_cast<long long>(timing.sortMs)
                << "ms | path=" << timing.searchPath << " candidates=" << timing.candidates
                << " phase1=" << timing.nameMatches
                << " phase2=" << timing.pathMatches
                << " results=" << timing.resultCount << " totalRecords=" << timing.totalRecords);
        } else {
            LOG_INFO("SearchEngine", "Query \"" << keyword << "\" total=" << ms
                << "ms lock_wait=" << static_cast<long long>(timing.lockWaitMs) << "ms scan=" << static_cast<long long>(timing.phase2Ms)
                << "ms lock_held=" << static_cast<long long>(timing.lockHeldMs) << "ms sort=" << static_cast<long long>(timing.sortMs)
                << "ms | path=linear results=" << timing.resultCount << " totalRecords=" << timing.totalRecords);
        }
    }

    return result;
}
