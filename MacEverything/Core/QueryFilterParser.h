#pragma once
#include "QueryAST.h"
#include "QueryDateParser.h"
#include <string>
#include <cctype>
#include <cstdint>
#include <algorithm>

/// Parse filter arguments and populate structured fields on QueryNode.
/// Called after the parser creates a FILTER node from raw filterName + filterArg.
class QueryFilterParser {
public:
    /// Post-process a FILTER node to parse its arguments.
    /// Fills in op, numVal1, numVal2, extList as appropriate.
    static void parse(QueryNode& node) {
        if (node.type != QueryNodeType::FILTER) return;

        const auto& name = node.filterName;
        const auto& arg = node.filterArg;

        if (name == "ext") {
            parseExt(node, arg);
        } else if (name == "size") {
            parseSize(node, arg);
        } else if (name == "len" || name == "depth") {
            parseNumeric(node, arg);
        } else if (name == "file" || name == "folder" || name == "type") {
            // No argument parsing needed for file:/folder:
            // type: arg is stored as-is in filterArg
        } else if (name == "dm" || name == "datemodified" ||
                   name == "dc" || name == "datecreated" ||
                   name == "da" || name == "dateaccessed") {
            QueryDateParser::parse(node, arg);
        } else if (name == "path" || name == "nopath" || name == "parent") {
            // Argument is a substring to match — stored as-is in filterArg
            // Lowercase it for case-insensitive matching
            std::string lower;
            for (char c : arg) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            node.filterArg = lower;
        }
    }

private:
    /// Parse ext:cpp or ext:cpp;h;hpp into extList
    static void parseExt(QueryNode& node, const std::string& arg) {
        node.op = CompareOp::EQ;
        std::string lower;
        for (char c : arg) {
            if (c == ';') {
                if (!lower.empty()) node.extList.push_back(lower);
                lower.clear();
            } else {
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        if (!lower.empty()) node.extList.push_back(lower);
    }

    /// Parse size:>1mb, size:<500kb, size:>=1gb, size:100kb..1mb, size:1024
    static void parseSize(QueryNode& node, const std::string& arg) {
        parseNumericWithUnits(node, arg);
    }

    /// Parse a plain numeric comparison: len:>10, depth:<3, depth:5
    static void parseNumeric(QueryNode& node, const std::string& arg) {
        parseNumericWithUnits(node, arg);
    }

    /// Unified numeric parser: handles comparison operators, ranges, and size units.
    /// Supports: >N, >=N, <N, <=N, N..M (range), N (exact equality)
    static void parseNumericWithUnits(QueryNode& node, const std::string& arg) {
        if (arg.empty()) return;

        // Check for range: N..M
        auto dotdot = arg.find("..");
        if (dotdot != std::string::npos) {
            node.op = CompareOp::RANGE;
            node.numVal1 = parseValueWithUnit(arg.substr(0, dotdot));
            node.numVal2 = parseValueWithUnit(arg.substr(dotdot + 2));
            return;
        }

        // Check for comparison operators
        size_t valStart = 0;
        if (arg.size() >= 2 && arg[0] == '>' && arg[1] == '=') {
            node.op = CompareOp::GE;
            valStart = 2;
        } else if (arg.size() >= 2 && arg[0] == '<' && arg[1] == '=') {
            node.op = CompareOp::LE;
            valStart = 2;
        } else if (arg[0] == '>') {
            node.op = CompareOp::GT;
            valStart = 1;
        } else if (arg[0] == '<') {
            node.op = CompareOp::LT;
            valStart = 1;
        } else {
            node.op = CompareOp::EQ;
        }

        node.numVal1 = parseValueWithUnit(arg.substr(valStart));
    }

    /// Parse a numeric value with optional size unit suffix.
    /// Supports: b, kb, mb, gb, tb (case insensitive)
    static uint64_t parseValueWithUnit(const std::string& s) {
        if (s.empty()) return 0;

        // Find where digits end and unit begins
        size_t numEnd = 0;
        while (numEnd < s.size() && (std::isdigit(static_cast<unsigned char>(s[numEnd])) || s[numEnd] == '.')) {
            numEnd++;
        }

        if (numEnd == 0) return 0;

        double value = std::stod(s.substr(0, numEnd));

        // Parse unit suffix
        std::string unit;
        for (size_t i = numEnd; i < s.size(); i++) {
            unit += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        }

        uint64_t multiplier = 1;
        if (unit == "kb" || unit == "k") {
            multiplier = 1024ULL;
        } else if (unit == "mb" || unit == "m") {
            multiplier = 1024ULL * 1024;
        } else if (unit == "gb" || unit == "g") {
            multiplier = 1024ULL * 1024 * 1024;
        } else if (unit == "tb" || unit == "t") {
            multiplier = 1024ULL * 1024 * 1024 * 1024;
        }
        // "b" or empty = bytes, no multiplier

        return static_cast<uint64_t>(value * multiplier);
    }
};
