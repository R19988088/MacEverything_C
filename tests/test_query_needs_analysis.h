#pragma once
#include "test_helpers.h"
#include "../MacEverything/Core/QueryNeedsAnalysis.h"
#include "../MacEverything/Core/QueryAST.h"
#include <iostream>

inline void runQueryNeedsAnalysisTests() {
    std::cout << "\n── Part 62: QueryNeedsAnalysis Tests ──\n\n";

    // 62a: Pure TERM → needsName + needsPath, not a pure filter
    {
        auto node = QueryNode::makeTerm("hello");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsName == true);
        CHECK(needs.needsPath == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "  62a: TERM needs name+path        PASS\n";
    }

    // 62b: size: filter → isPureFilter, needsSize
    {
        auto node = QueryNode::makeFilter("size", ">1mb");
        node->op = CompareOp::GT;
        node->numVal1 = 1048576;
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsSize == true);
        CHECK(needs.needsName == false);
        CHECK(needs.needsPath == false);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62b: size: is pure filter        PASS\n";
    }

    // 62c: file: filter → isPureFilter, needsType
    {
        auto node = QueryNode::makeFilter("file", "");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsType == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62c: file: is pure filter        PASS\n";
    }

    // 62d: folder: filter → isPureFilter, needsType
    {
        auto node = QueryNode::makeFilter("folder", "");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsType == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62d: folder: is pure filter      PASS\n";
    }

    // 62e: dm: filter → isPureFilter, needsModTime
    {
        auto node = QueryNode::makeFilter("dm", "today");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsModTime == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62e: dm: is pure filter          PASS\n";
    }

    // 62f: ext: filter → needsName, NOT a pure filter
    {
        auto node = QueryNode::makeFilter("ext", "txt");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsName == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "  62f: ext: needs name             PASS\n";
    }

    // 62g: path: filter → needsPath, NOT a pure filter
    {
        auto node = QueryNode::makeFilter("path", "/usr/local");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsPath == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "  62g: path: needs path            PASS\n";
    }

    // 62h: AND(TERM, size:) → NOT pure filter (TERM requires name+path)
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        kids.push_back(QueryNode::makeTerm("hello"));
        auto sizeNode = QueryNode::makeFilter("size", ">1mb");
        sizeNode->op = CompareOp::GT;
        sizeNode->numVal1 = 1048576;
        kids.push_back(std::move(sizeNode));
        auto andNode = QueryNode::makeAnd(std::move(kids));

        QueryNeeds needs = analyzeQueryNeeds(*andNode);
        CHECK(needs.needsName == true);
        CHECK(needs.needsPath == true);
        CHECK(needs.needsSize == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "  62h: AND(TERM,size:) not pure    PASS\n";
    }

    // 62i: OR(size:, dm:) → isPureFilter
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        auto sizeNode = QueryNode::makeFilter("size", ">1mb");
        sizeNode->op = CompareOp::GT;
        sizeNode->numVal1 = 1048576;
        kids.push_back(std::move(sizeNode));
        auto dmNode = QueryNode::makeFilter("dm", "today");
        dmNode->op = CompareOp::GT;
        dmNode->numVal1 = 1000;
        kids.push_back(std::move(dmNode));
        auto orNode = QueryNode::makeOr(std::move(kids));

        QueryNeeds needs = analyzeQueryNeeds(*orNode);
        CHECK(needs.needsSize == true);
        CHECK(needs.needsModTime == true);
        CHECK(needs.needsName == false);
        CHECK(needs.needsPath == false);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62i: OR(size:,dm:) is pure       PASS\n";
    }

    // 62j: NOT(file:) → isPureFilter, needsType
    {
        auto fileNode = QueryNode::makeFilter("file", "");
        auto notNode = QueryNode::makeNot(std::move(fileNode));

        QueryNeeds needs = analyzeQueryNeeds(*notNode);
        CHECK(needs.needsType == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62j: NOT(file:) is pure filter   PASS\n";
    }

    // 62k: AND(file:, size:, dm:) → isPureFilter, all filter flags set
    {
        std::vector<std::unique_ptr<QueryNode>> kids;
        kids.push_back(QueryNode::makeFilter("file", ""));
        auto sizeNode = QueryNode::makeFilter("size", ">100");
        sizeNode->op = CompareOp::GT;
        sizeNode->numVal1 = 100;
        kids.push_back(std::move(sizeNode));
        auto dmNode = QueryNode::makeFilter("dm", "today");
        dmNode->op = CompareOp::GT;
        dmNode->numVal1 = 1000;
        kids.push_back(std::move(dmNode));
        auto andNode = QueryNode::makeAnd(std::move(kids));

        QueryNeeds needs = analyzeQueryNeeds(*andNode);
        CHECK(needs.needsType == true);
        CHECK(needs.needsSize == true);
        CHECK(needs.needsModTime == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62k: AND(file:,size:,dm:) pure   PASS\n";
    }

    // 62l: len: filter → needsName
    {
        auto node = QueryNode::makeFilter("len", ">10");
        node->op = CompareOp::GT;
        node->numVal1 = 10;
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsName == true);
        CHECK(needs.isPureFilter() == false);
        std::cout << "  62l: len: needs name             PASS\n";
    }

    // 62m: type: filter → needsType, isPureFilter
    {
        auto node = QueryNode::makeFilter("type", "file");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsType == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62m: type: is pure filter        PASS\n";
    }

    // 62n: datemodified: (long form) → needsModTime
    {
        auto node = QueryNode::makeFilter("datemodified", "today");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsModTime == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62n: datemodified: pure filter   PASS\n";
    }

    // 62o: datecreated: → needsModTime
    {
        auto node = QueryNode::makeFilter("datecreated", "today");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsModTime == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62o: datecreated: pure filter    PASS\n";
    }

    // 62p: dateaccessed: → needsModTime
    {
        auto node = QueryNode::makeFilter("dateaccessed", "today");
        QueryNeeds needs = analyzeQueryNeeds(*node);
        CHECK(needs.needsModTime == true);
        CHECK(needs.isPureFilter() == true);
        std::cout << "  62p: dateaccessed: pure filter   PASS\n";
    }
}
