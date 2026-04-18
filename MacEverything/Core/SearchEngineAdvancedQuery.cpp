#include "SearchEngine.h"
#include "StringUtils.h"
#include "QueryParser.h"
#include "QueryTokenizer.h"
#include "QueryFilterParser.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <chrono>

// ---------------------------------------------------------------------------
// queryAdvanced: evaluate a parsed AST against the record set.
//
// Strategy:
// 1. Parse the query into an AST.
// 2. Extract best TERM for trigram pre-filtering.
// 3. Evaluate AST per-record: TERM=substring/glob, FILTER=structured filters,
//    AND/OR/NOT=boolean logic.
// 4. Score, sort, and return results.
//
// Supported FILTER types: ext:, size:, file:, folder:, type:, path:, nopath:,
// parent:, depth:, len:
// ---------------------------------------------------------------------------

namespace {

/// Compare a numeric value against a filter's parsed operator and thresholds.
static bool compareNumeric(uint64_t val, CompareOp op, uint64_t v1, uint64_t v2) {
    switch (op) {
        case CompareOp::EQ:    return val == v1;
        case CompareOp::LT:    return val < v1;
        case CompareOp::LE:    return val <= v1;
        case CompareOp::GT:    return val > v1;
        case CompareOp::GE:    return val >= v1;
        case CompareOp::RANGE: return val >= v1 && val <= v2;
    }
    return false;
}

/// Extract lowercase file extension from name data.
/// Returns empty string_view if no extension found.
static std::string getExtLower(const char* nameData, uint16_t nameLen) {
    // Find last '.'
    int dotPos = -1;
    for (int i = static_cast<int>(nameLen) - 1; i >= 0; --i) {
        if (nameData[i] == '.') { dotPos = i; break; }
    }
    if (dotPos < 0 || dotPos == static_cast<int>(nameLen) - 1) return {};
    // nameData is already lowercase
    return std::string(nameData + dotPos + 1, nameLen - dotPos - 1);
}

/// Count '/' in path to determine depth.
static uint64_t computeDepth(const char* pathData, uint16_t pathLen) {
    uint64_t depth = 0;
    for (uint16_t i = 0; i < pathLen; ++i) {
        if (pathData[i] == '/') depth++;
    }
    return depth;
}

/// Evaluate a FILTER node against a single record.
static bool evalFilter(const QueryNode& node,
                       const FileRecord& rec,
                       const char* nameData, uint16_t nameLen,
                       const char* pathData, uint16_t pathLen,
                       std::vector<char>& pathBuf) {
    const auto& name = node.filterName;

    // ext: — match file extension
    if (name == "ext") {
        std::string ext = getExtLower(nameData, nameLen);
        if (ext.empty()) return false;
        for (auto& e : node.extList) {
            if (ext == e) return true;
        }
        return false;
    }

    // size: — compare file size
    if (name == "size") {
        return compareNumeric(rec.size, node.op, node.numVal1, node.numVal2);
    }

    // file: — match files only
    if (name == "file") {
        return rec.type == 1; // 1=file
    }

    // folder: — match directories only
    if (name == "folder") {
        return rec.type == 2; // 2=dir
    }

    // type: — shorthand for file/folder
    if (name == "type") {
        if (node.filterArg == "file") return rec.type == 1;
        if (node.filterArg == "folder" || node.filterArg == "dir") return rec.type == 2;
        return false;
    }

    // path: — path must contain substring (case insensitive)
    if (name == "path") {
        // Build full path: pathData + '/' + nameData
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen,
                                node.filterArg.data(), node.filterArg.size());
    }

    // nopath: — path must NOT contain substring
    if (name == "nopath") {
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return !me::simdContains(pathBuf.data(), fullLen,
                                 node.filterArg.data(), node.filterArg.size());
    }

    // parent: — direct parent path must match exactly
    if (name == "parent") {
        // filterArg is lowercased; pathData is already lowercased
        std::string pathStr(pathData, pathLen);
        return pathStr == node.filterArg;
    }

    // depth: — directory depth (count of '/' in full path)
    if (name == "depth") {
        uint64_t depth = computeDepth(pathData, pathLen);
        return compareNumeric(depth, node.op, node.numVal1, node.numVal2);
    }

    // len: — filename length
    if (name == "len") {
        return compareNumeric(static_cast<uint64_t>(nameLen), node.op,
                              node.numVal1, node.numVal2);
    }

    // Unknown filter — pass through
    return true;
}

/// Evaluate a TERM node against a single record.
/// Returns true if the record's name or full path matches the term text.
static bool evalTerm(const QueryNode& node,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     std::vector<char>& pathBuf) {
    std::string lower = me::toLower(node.text);
    // Check name match
    if (me::simdContains(nameData, nameLen, lower.data(), lower.size())) {
        return true;
    }
    // Check full path match
    size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
    if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
    memcpy(pathBuf.data(), pathData, pathLen);
    pathBuf[pathLen] = '/';
    memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
    return me::simdContains(pathBuf.data(), fullLen, lower.data(), lower.size());
}

/// Recursively evaluate AST node against a single record.
static bool evalNode(const QueryNode& node,
                     const FileRecord& rec,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     std::vector<char>& pathBuf) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return evalTerm(node, nameData, nameLen, pathData, pathLen, pathBuf);

        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNode(*child, rec, nameData, nameLen, pathData, pathLen, pathBuf))
                    return false;
            }
            return true;

        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNode(*child, rec, nameData, nameLen, pathData, pathLen, pathBuf))
                    return true;
            }
            return false;

        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNode(*node.children[0], rec, nameData, nameLen, pathData, pathLen, pathBuf);

        case QueryNodeType::FILTER:
            return evalFilter(node, rec, nameData, nameLen, pathData, pathLen, pathBuf);
    }
    return false;
}

/// Extract all TERM texts from the AST (for trigram pre-filtering).
static void collectTerms(const QueryNode& node, std::vector<std::string>& terms) {
    switch (node.type) {
        case QueryNodeType::TERM:
            terms.push_back(me::toLower(node.text));
            break;
        case QueryNodeType::AND:
        case QueryNodeType::OR:
            for (auto& child : node.children) collectTerms(*child, terms);
            break;
        case QueryNodeType::NOT:
            if (!node.children.empty()) collectTerms(*node.children[0], terms);
            break;
        case QueryNodeType::FILTER:
            break;
    }
}

/// Find the single best (shortest) term from the AND-level for trigram pre-filtering.
/// For OR queries we can't pre-filter (any branch might match), so return empty.
static std::string bestTrigramTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM) {
        return me::toLower(node.text);
    }
    if (node.type == QueryNodeType::AND) {
        // Pick the longest TERM child (more trigrams = better selectivity)
        std::string best;
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::TERM) {
                std::string t = me::toLower(child->text);
                if (t.size() > best.size()) best = t;
            }
        }
        return best;
    }
    // OR, NOT, FILTER — can't reliably pre-filter
    return {};
}

} // anonymous namespace

std::vector<uint32_t> SearchEngine::queryAdvanced(const std::string& input,
                                                   uint32_t maxResults,
                                                   bool useTrigram,
                                                   QueryTimingInfo& timing) const {
    auto queryStart = std::chrono::steady_clock::now();

    // Parse the AST
    auto ast = QueryParser::parse(input);
    if (!ast) return {};

    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (records_.empty()) return {};
    size_t totalSize = records_.size();
    uint64_t myGen = queryGeneration_.load(std::memory_order_relaxed);

    auto beforeTrigram = afterLock, afterTrigram = afterLock;

    // Try to use trigram index to narrow down candidates.
    // Extract the best term from the top-level AND for pre-filtering.
    std::string trigramKey = bestTrigramTerm(*ast);
    std::vector<uint32_t> candidates;
    bool useTrigramIndex = false;

    if (useTrigram && !trigramKey.empty() && trigramKey.size() >= 3 && !nameTrigramIndex_.empty()) {
        beforeTrigram = std::chrono::steady_clock::now();
        bool allFound = false;
        candidates = intersectPostingLists(nameTrigramIndex_, trigramKey, allFound);
        if (allFound && candidates.size() <= totalSize / 67) {
            useTrigramIndex = true;
        } else {
            candidates.clear();
        }
        afterTrigram = std::chrono::steady_clock::now();
    }

    // Evaluate AST against each candidate (or all records for linear scan)
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };
    std::vector<Match> merged;
    std::vector<char> pathBuf;

    auto evalRecord = [&](uint32_t idx) {
        if (records_[idx].type == 0) return;
        const char* nd = namePool_.data(idx);
        uint16_t nl = namePool_.length(idx);
        uint32_t pi = pathIndices_[idx];
        const char* pd = lowerPathPool_.data(pi);
        uint16_t pl = lowerPathPool_.length(pi);

        if (!evalNode(*ast, records_[idx], nd, nl, pd, pl, pathBuf)) return;

        // Compute priority: 0=exact, 1=starts-with, 2=contains name, 3=path-only
        std::string lowerInput = me::toLower(input);
        uint8_t priority = 2;
        if (me::simdContains(nd, nl, lowerInput.data(), lowerInput.size())) {
            priority = namePriority(nd, nl, lowerInput.data(), lowerInput.size());
        } else {
            priority = 3;
        }
        uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
        merged.push_back({idx, priority, pLen});
    };

    auto beforePhase = std::chrono::steady_clock::now();

    if (useTrigramIndex) {
        for (size_t ci = 0; ci < candidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            evalRecord(candidates[ci]);
        }
    } else {
        for (uint32_t idx = 0; idx < totalSize; idx++) {
            if ((idx & 4095) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            evalRecord(idx);
        }
    }

    auto afterPhase = std::chrono::steady_clock::now();

    // Release lock before sorting
    auto beforeUnlock = std::chrono::steady_clock::now();
    lock.unlock();

    // Sort by priority, then path length
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

    // Populate timing
    auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
    timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
    timing.lockWaitMs = toMs(afterLock - beforeLock);
    timing.lockHeldMs = toMs(beforeUnlock - afterLock);
    timing.sortMs = toMs(afterSort - beforeSort);
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = 0;
    timing.phase2Ms = toMs(afterPhase - beforePhase);
    timing.totalRecords = totalSize;
    timing.candidates = candidates.size();
    timing.nameMatches = 0;
    timing.pathMatches = 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex;
    timing.searchPath = useTrigramIndex ? "advanced-trigram" : "advanced-linear";

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        LOG_INFO("SearchEngine", "QueryAdvanced \"" << input << "\" total=" << ms
            << "ms | path=" << timing.searchPath
            << " candidates=" << timing.candidates
            << " results=" << timing.resultCount
            << " totalRecords=" << timing.totalRecords);
    }

    return result;
}
