#include "SearchEngine.h"
#include "StringUtils.h"
#include "QueryParser.h"
#include "QueryTokenizer.h"
#include "QueryFilterParser.h"
#include "QueryNeedsAnalysis.h"
#include "ASTStructuredTransform.h"
#include "ASTGlobTransform.h"
#include "CompiledGlob.h"
#include "SIMDSearch.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <regex>
#include <functional>
#include <thread>
#include <dispatch/dispatch.h>

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

// globMatchImpl is now in CompiledGlob.h

/// Check if a character is a word boundary character (not alphanumeric).
/// Note: underscore '_' IS a boundary (consistent with Everything behavior).
static bool isWordBoundaryChar(char c) {
    return !std::isalnum(static_cast<unsigned char>(c));
}

/// Check if a substring match at position `pos` of length `len` within `text`
/// (total length `textLen`) is a whole-word match.
static bool isWholeWordMatch(const char* text, size_t textLen,
                             size_t pos, size_t len) {
    // Check left boundary: must be start of string or preceded by non-word char
    if (pos > 0 && !isWordBoundaryChar(text[pos - 1])) return false;
    // Check right boundary: must be end of string or followed by non-word char
    size_t end = pos + len;
    if (end < textLen && !isWordBoundaryChar(text[end])) return false;
    return true;
}

/// Pre-compiled regex cache, keyed by QueryNode pointer.
using RegexCache = std::unordered_map<const QueryNode*, std::regex>;

/// Collect all REGEX TERM nodes from the AST for pre-compilation.
static void collectRegexNodes(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::REGEX) {
        out.push_back(&node);
    }
    for (auto& child : node.children) {
        collectRegexNodes(*child, out);
    }
}

/// Evaluate a FILTER node against a single record.
static bool evalFilter(const QueryNode& node,
                       const FileRecord& rec,
                       const char* nameData, uint16_t nameLen,
                       const char* pathData, uint16_t pathLen,
                       std::vector<char>& pathBuf) {
    const auto& name = node.filterName;

    // __pathseg: internal filter — structured path segment matching
    if (name == "__pathseg") {
        // Build full path: pathData/nameData for component-level matching
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        std::string_view fullPath(pathBuf.data(), fullLen);
        return SearchEngine::pathSegmentsMatch(fullPath, node.pathSegments);
    }

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

    // dm: / datemodified: — modification date filter
    if (name == "dm" || name == "datemodified") {
        return compareNumeric(static_cast<uint64_t>(rec.modTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // dc: / datecreated: — creation date (uses modTime as fallback since
    // FileRecord doesn't store birthtime yet)
    if (name == "dc" || name == "datecreated") {
        return compareNumeric(static_cast<uint64_t>(rec.modTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // da: / dateaccessed: — access date (uses modTime as fallback)
    if (name == "da" || name == "dateaccessed") {
        return compareNumeric(static_cast<uint64_t>(rec.modTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // Unknown filter — pass through
    return true;
}

/// Evaluate a TERM node against a single record.
/// Returns true if the record's name or full path matches the term text.
/// nameData is lowercase (from namePool_), rec.name has original case.
static bool evalTerm(const QueryNode& node,
                     const FileRecord& rec,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    switch (node.mode) {

    case MatchMode::SUBSTRING: {
        if (node.caseSensitive) {
            // Case-sensitive: compare against original-case name (rec.name)
            const auto& origName = rec.name;
            const auto& term = node.text;
            // Check name
            if (origName.size() >= term.size()) {
                for (size_t i = 0; i + term.size() <= origName.size(); ++i) {
                    if (memcmp(origName.data() + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            // Check full path (original case: rec.path + "/" + rec.name)
            if (node.nameOnly) return false;
            std::string fullPath = rec.path + "/" + rec.name;
            if (fullPath.size() >= term.size()) {
                for (size_t i = 0; i + term.size() <= fullPath.size(); ++i) {
                    if (memcmp(fullPath.data() + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            return false;
        }
        // Case-insensitive (default): use lowercase nameData
        const auto& lower = node.textLower;
        if (me::simdContains(nameData, nameLen, lower.data(), lower.size())) {
            return true;
        }
        // Skip full-path matching when nameOnly is set
        // (transformSlashTerms uses this for name-component terms)
        if (node.nameOnly) return false;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen, lower.data(), lower.size());
    }

    case MatchMode::GLOB: {
        const auto& pattern = node.textLower;
        // If pattern contains '/', match against full path; otherwise name only
        if (pattern.find('/') != std::string::npos) {
            size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
            if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
            memcpy(pathBuf.data(), pathData, pathLen);
            pathBuf[pathLen] = '/';
            memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
            std::string fullPath(pathBuf.data(), fullLen);
            return globMatchImpl(pattern, fullPath);
        }
        std::string name(nameData, nameLen);
        return globMatchImpl(pattern, name);
    }

    case MatchMode::REGEX: {
        auto it = regexCache.find(&node);
        if (it == regexCache.end()) return false;
        // Match against lowercase name by default
        std::string name(nameData, nameLen);
        return std::regex_search(name, it->second);
    }

    case MatchMode::WHOLEWORD: {
        const auto& lower = node.textLower;
        // Check in lowercase name
        std::string name(nameData, nameLen);
        for (size_t i = 0; i + lower.size() <= name.size(); ++i) {
            if (memcmp(name.data() + i, lower.data(), lower.size()) == 0) {
                if (isWholeWordMatch(name.data(), name.size(), i, lower.size()))
                    return true;
            }
        }
        return false;
    }

    case MatchMode::WHOLEFILENAME: {
        const auto& lower = node.textLower;
        // Entire filename must match
        if (nameLen == lower.size() &&
            memcmp(nameData, lower.data(), nameLen) == 0)
            return true;
        return false;
    }

    } // switch
    return false;
}

/// Recursively evaluate AST node against a single record.
static bool evalNode(const QueryNode& node,
                     const FileRecord& rec,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return evalTerm(node, rec, nameData, nameLen, pathData, pathLen, pathBuf, regexCache);

        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNode(*child, rec, nameData, nameLen, pathData, pathLen, pathBuf, regexCache))
                    return false;
            }
            return true;

        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNode(*child, rec, nameData, nameLen, pathData, pathLen, pathBuf, regexCache))
                    return true;
            }
            return false;

        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNode(*node.children[0], rec, nameData, nameLen, pathData, pathLen, pathBuf, regexCache);

        case QueryNodeType::FILTER:
            return evalFilter(node, rec, nameData, nameLen, pathData, pathLen, pathBuf);
    }
    return false;
}

/// Extract all TERM texts from the AST (for trigram pre-filtering).
static void collectTerms(const QueryNode& node, std::vector<std::string>& terms) {
    switch (node.type) {
        case QueryNodeType::TERM:
            terms.push_back(node.textLower);
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

/// Find the single best (longest) SUBSTRING TERM from the AND-level for trigram pre-filtering.
/// For OR queries we can't pre-filter (any branch might match), so return empty.
/// Skips __pathseg filter nodes and recurses into AND children.
static std::string bestTrigramTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM) {
        // Only SUBSTRING mode can use trigram pre-filtering
        if (node.mode != MatchMode::SUBSTRING) return {};
        return node.textLower;
    }
    if (node.type == QueryNodeType::AND) {
        // Pick the longest SUBSTRING TERM child (more trigrams = better selectivity)
        // Also recurse into nested AND children (from transformSlashTerms)
        std::string best;
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::TERM && child->mode == MatchMode::SUBSTRING) {
                const auto& t = child->textLower;
                if (t.size() > best.size()) best = t;
            } else if (child->type == QueryNodeType::AND) {
                std::string t = bestTrigramTerm(*child);
                if (t.size() > best.size()) best = t;
            }
        }
        return best;
    }
    // OR, NOT, FILTER — can't reliably pre-filter
    return {};
}

/// Extract continuous literal substrings (length >= 3) from a regex pattern.
/// Used for trigram pre-filtering of regex queries.
static std::vector<std::string> extractRegexLiteralsImpl(const std::string& pattern) {
    std::vector<std::string> literals;
    std::string current;
    bool inCharClass = false;

    for (size_t i = 0; i < pattern.size(); i++) {
        char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            char next = pattern[i + 1];
            // Escaped literal metacharacters
            if (next == '.' || next == '*' || next == '+' || next == '?' ||
                next == '(' || next == ')' || next == '[' || next == ']' ||
                next == '{' || next == '}' || next == '|' || next == '^' ||
                next == '$' || next == '\\' || next == '/') {
                if (!inCharClass) current += std::tolower(static_cast<unsigned char>(next));
                i++;
            } else {
                // \d, \w, \s, etc. — break current literal
                if (current.size() >= 3) literals.push_back(std::move(current));
                current.clear();
                i++;
            }
        } else if (c == '[') {
            if (current.size() >= 3) literals.push_back(std::move(current));
            current.clear();
            inCharClass = true;
        } else if (c == ']') {
            inCharClass = false;
        } else if (inCharClass) {
            // Inside character class — don't extract
        } else if (c == '.' || c == '*' || c == '+' || c == '?' ||
                   c == '(' || c == ')' || c == '{' || c == '}' ||
                   c == '|' || c == '^' || c == '$') {
            // Metacharacter — break current literal
            if (current.size() >= 3) literals.push_back(std::move(current));
            current.clear();
        } else {
            current += std::tolower(static_cast<unsigned char>(c));
        }
    }
    if (current.size() >= 3) literals.push_back(std::move(current));
    return literals;
}

/// Collect regex literal substrings from all REGEX TERM nodes in the AST.
static std::vector<std::string> extractRegexLiteralsFromAST(const QueryNode& node) {
    std::vector<std::string> allLiterals;
    std::function<void(const QueryNode&)> collect = [&](const QueryNode& n) {
        if (n.type == QueryNodeType::TERM && n.mode == MatchMode::REGEX) {
            auto lits = extractRegexLiteralsImpl(n.text);
            allLiterals.insert(allLiterals.end(), lits.begin(), lits.end());
        }
        for (auto& child : n.children) collect(*child);
    };
    collect(node);
    return allLiterals;
}

/// Evaluate a FILTER node using SoA columnar values (no string access).
/// Only handles type/size/modTime filters. Other filters return true (pass-through).
static bool evalFilterSoA(const QueryNode& node,
                          uint8_t type, uint64_t size, int64_t modTime) {
    const auto& name = node.filterName;

    if (name == "size") {
        return compareNumeric(size, node.op, node.numVal1, node.numVal2);
    }
    if (name == "file") {
        return type == 1;
    }
    if (name == "folder") {
        return type == 2;
    }
    if (name == "type") {
        if (node.filterArg == "file") return type == 1;
        if (node.filterArg == "folder" || node.filterArg == "dir") return type == 2;
        return false;
    }
    if (name == "dm" || name == "datemodified") {
        return compareNumeric(static_cast<uint64_t>(modTime), node.op,
                              node.numVal1, node.numVal2);
    }
    if (name == "dc" || name == "datecreated") {
        return compareNumeric(static_cast<uint64_t>(modTime), node.op,
                              node.numVal1, node.numVal2);
    }
    if (name == "da" || name == "dateaccessed") {
        return compareNumeric(static_cast<uint64_t>(modTime), node.op,
                              node.numVal1, node.numVal2);
    }
    return true;
}

/// Recursively evaluate AST node using SoA columnar values only (pure-filter fast path).
static bool evalNodeSoA(const QueryNode& node,
                        uint8_t type, uint64_t size, int64_t modTime) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return false; // Should never happen in pure-filter path
        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNodeSoA(*child, type, size, modTime)) return false;
            }
            return true;
        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNodeSoA(*child, type, size, modTime)) return true;
            }
            return false;
        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNodeSoA(*node.children[0], type, size, modTime);
        case QueryNodeType::FILTER:
            return evalFilterSoA(node, type, size, modTime);
    }
    return false;
}

/// Extract the first SUBSTRING TERM text from the AST for result scoring.
/// Returns empty string if no suitable term is found (pure filter queries).
/// Skips __pathseg filter nodes (they are path constraints, not scoring terms).
static std::string extractScoringTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::SUBSTRING) {
        return node.textLower;
    }
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        return {}; // path segment constraint, not a scoring term
    }
    if (node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) {
        for (auto& child : node.children) {
            auto t = extractScoringTerm(*child);
            if (!t.empty()) return t;
        }
    }
    return {};
}

/// Extract the best (longest, >= 3 chars) path segment text from __pathseg filters in the AST.
/// Only looks in AND-level nodes (can't pre-filter OR branches).
static std::string bestPathSegTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        std::string best;
        for (auto& seg : node.pathSegments) {
            if (seg.text.size() >= 3 && seg.text.size() > best.size()) {
                best = seg.text;
            }
        }
        return best;
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            auto t = bestPathSegTerm(*child);
            if (t.size() > best.size()) best = t;
        }
        return best;
    }
    return {};
}

} // anonymous namespace

// Expose extractRegexLiterals for unit testing
namespace me_test {
    std::vector<std::string> extractRegexLiterals(const std::string& pattern) {
        return extractRegexLiteralsImpl(pattern);
    }
}

std::vector<uint32_t> SearchEngine::queryAdvanced(const std::string& input,
                                                   uint32_t maxResults,
                                                   bool useTrigram,
                                                   QueryTimingInfo& timing) const {
    auto queryStart = std::chrono::steady_clock::now();

    // Parse the AST
    auto ast = QueryParser::parse(input);
    if (!ast) return {};

    // Transform TERM nodes containing '/' into structured path-segment constraints.
    // e.g. TERM("/usr/local/test") → AND(FILTER("__pathseg",[usr,local]), TERM("test"))
    ast = transformSlashTerms(std::move(ast));

    // Transform TERM nodes containing '*' or '?' from SUBSTRING to GLOB mode.
    ast = transformGlobTerms(std::move(ast));

    // Analyze which record fields the query actually needs
    QueryNeeds needs = analyzeQueryNeeds(*ast);

    // Pre-compile all regex patterns before acquiring lock
    RegexCache regexCache;
    {
        std::vector<const QueryNode*> regexNodes;
        collectRegexNodes(*ast, regexNodes);
        for (auto* rn : regexNodes) {
            try {
                regexCache.emplace(rn, std::regex(rn->text,
                    std::regex::ECMAScript | std::regex::icase | std::regex::optimize));
            } catch (const std::regex_error&) {
                // Invalid regex — evalTerm will return false for this node
            }
        }
    }

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

    // Regex trigram pre-filtering: extract literal substrings from regex patterns
    if (useTrigram && !useTrigramIndex && !nameTrigramIndex_.empty()) {
        auto regexLiterals = extractRegexLiteralsFromAST(*ast);
        if (!regexLiterals.empty()) {
            beforeTrigram = std::chrono::steady_clock::now();
            bool allFound = false;
            candidates = intersectPostingListsMulti(nameTrigramIndex_, regexLiterals, allFound);
            if (allFound && candidates.size() <= totalSize / 67) {
                useTrigramIndex = true;
            } else {
                candidates.clear();
            }
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Path trigram pre-filtering: if AST has __pathseg with a segment >= 3 chars,
    // use pathTrigramIndex_ to narrow candidates via path indices → record indices.
    if (useTrigram && !useTrigramIndex && !pathTrigramIndex_.empty()) {
        std::string pathSegKey = bestPathSegTerm(*ast);
        if (pathSegKey.size() >= 3) {
            beforeTrigram = std::chrono::steady_clock::now();
            bool allFound = false;
            auto pathCandidates = intersectPostingLists(pathTrigramIndex_, pathSegKey, allFound);
            if (allFound) {
                // Expand path indices to record indices
                for (uint32_t pi : pathCandidates) {
                    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
                    if (pi >= pathIdxToRecords_.size()) continue;
                    const auto& recIds = pathIdxToRecords_[pi];
                    candidates.insert(candidates.end(), recIds.begin(), recIds.end());
                }
                // Sort and deduplicate
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
                if (candidates.size() <= totalSize / 4) {
                    useTrigramIndex = true;
                } else {
                    candidates.clear();
                }
            }
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Evaluate AST against each candidate (or all records for linear scan)
    // Uses SearchEngine::Match (declared in SearchEngine.h)
    std::vector<Match> merged;
    std::vector<char> pathBuf;

    // Extract scoring term from AST (not raw input) — hoisted out of per-record loop
    std::string scoringTerm = extractScoringTerm(*ast);

    auto beforePhase = std::chrono::steady_clock::now();

    if (useTrigramIndex) {
        // Trigram-filtered path: candidates are already small, single-threaded
        for (size_t ci = 0; ci < candidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            uint32_t idx = candidates[ci];
            if (records_[idx].type == 0) continue;
            const char* nd = namePool_.data(idx);
            uint16_t nl = namePool_.length(idx);
            uint32_t pi = pathIndices_[idx];
            const char* pd = lowerPathPool_.data(pi);
            uint16_t pl = lowerPathPool_.length(pi);
            if (!evalNode(*ast, records_[idx], nd, nl, pd, pl, pathBuf, regexCache)) continue;
            uint8_t priority = 2;
            if (!scoringTerm.empty()) {
                if (me::simdContains(nd, nl, scoringTerm.data(), scoringTerm.size())) {
                    priority = namePriority(nd, nl, scoringTerm.data(), scoringTerm.size());
                } else {
                    priority = 3;
                }
            }
            merged.push_back({idx, priority, static_cast<uint32_t>(pl + 1 + nl)});
        }
    } else {
        // Linear scan — parallelize with GCD dispatch_apply
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        // For small record sets, don't over-parallelize
        if (totalSize < 10000) numThreads = 1;

        size_t chunkSize = (totalSize + numThreads - 1) / numThreads;
        __block std::vector<std::vector<Match>> threadResults(numThreads);

        const auto* genPtr = &queryGeneration_;
        uint64_t capturedGen = myGen;
        bool pureFilter = needs.isPureFilter();

        // Capture references for block
        const auto* astPtr = ast.get();
        const auto* typesPtr = types_.data();
        const auto* sizesPtr = sizes_.data();
        const auto* modTimesPtr = modTimes_.data();
        const auto& records = records_;
        const auto& namePool = namePool_;
        const auto& lowerPathPool = lowerPathPool_;
        const auto& pIndices = pathIndices_;
        const auto& regCache = regexCache;
        const auto& sTerm = scoringTerm;

        dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_apply(numThreads, queue, ^(size_t t) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, totalSize);
            if (start >= end) return;

            auto& local = threadResults[t];
            std::vector<char> localPathBuf;

            if (pureFilter) {
                // ── SIMD-batched pure-filter fast path ──
                // Process 16 records at a time: batch liveness via simdTypeLive16,
                // then evaluate surviving records through the SoA AST evaluator.
                size_t idx = start;

                // Align to 16-byte boundary for SIMD loads
                size_t alignedStart = (start + 15) & ~size_t(15);
                if (alignedStart > end) alignedStart = end;

                // Scalar pre-amble (before alignment)
                for (; idx < alignedStart; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNodeSoA(*astPtr, typesPtr[idx], sizesPtr[idx], modTimesPtr[idx])) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }

                // SIMD main loop: 16 records per iteration
                for (; idx + 16 <= end; idx += 16) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    // Batch liveness: skip chunk if all 16 are tombstones
                    uint16_t liveMask = me::simdTypeLive16(typesPtr + idx);
                    if (liveMask == 0) continue;

                    // Process live records in this chunk
                    while (liveMask) {
                        int bit = __builtin_ctz(liveMask);
                        size_t ri = idx + bit;
                        if (evalNodeSoA(*astPtr, typesPtr[ri], sizesPtr[ri], modTimesPtr[ri])) {
                            local.push_back({static_cast<uint32_t>(ri), 2, 0});
                        }
                        liveMask &= liveMask - 1;
                    }
                }

                // Scalar tail
                for (; idx < end; idx++) {
                    if (typesPtr[idx] == 0) continue;
                    if (!evalNodeSoA(*astPtr, typesPtr[idx], sizesPtr[idx], modTimesPtr[idx])) continue;
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                }
            } else {
                // ── Full evaluation path (needs string access) ──
                for (size_t idx = start; idx < end; idx++) {
                    if ((idx & 4095) == 0 && genPtr->load(std::memory_order_relaxed) != capturedGen) return;

                    if (records[idx].type == 0) continue;
                    const char* nd = namePool.data(static_cast<uint32_t>(idx));
                    uint16_t nl = namePool.length(static_cast<uint32_t>(idx));
                    uint32_t pi = pIndices[idx];
                    const char* pd = lowerPathPool.data(pi);
                    uint16_t pl = lowerPathPool.length(pi);

                    if (!evalNode(*astPtr, records[idx], nd, nl, pd, pl, localPathBuf, regCache)) continue;

                    uint8_t priority = 2;
                    if (!sTerm.empty()) {
                        if (me::simdContains(nd, nl, sTerm.data(), sTerm.size())) {
                            priority = namePriority(nd, nl, sTerm.data(), sTerm.size());
                        } else {
                            priority = 3;
                        }
                    }
                    uint32_t pLen = static_cast<uint32_t>(pl + 1 + nl);
                    local.push_back({static_cast<uint32_t>(idx), priority, pLen});
                }
            }
        });

        // Check if superseded after dispatch_apply
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

        // Merge thread-local results
        for (auto& v : threadResults) {
            merged.insert(merged.end(), v.begin(), v.end());
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
    if (useTrigramIndex) {
        if (!trigramKey.empty()) {
            timing.searchPath = "advanced-trigram";
        } else if (!bestPathSegTerm(*ast).empty()) {
            timing.searchPath = "advanced-path-trigram";
        } else {
            timing.searchPath = "advanced-regex-trigram";
        }
    } else if (needs.isPureFilter()) {
        timing.searchPath = "advanced-pure-filter-soa-gcd";
    } else {
        timing.searchPath = "advanced-linear-gcd";
    }

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
