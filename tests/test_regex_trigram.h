// Part 60 — Regex trigram pre-filtering tests
// Tests that regex queries extract literal substrings and use trigram index
// to pre-filter candidates instead of full linear scan.

#pragma once

#include <vector>
#include <string>

// Forward-declare the function from anonymous namespace — we test it via
// a wrapper exposed only for testing (see SearchEngineAdvancedQuery.cpp).
namespace me_test {
    std::vector<std::string> extractRegexLiterals(const std::string& pattern);
}

static void runRegexTrigramTests() {
    std::cout << "\n╔══════════════════════════════════════════╗\n";
    std::cout << "║  Part 60: Regex Trigram Pre-filtering    ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    // ── Test 1: Basic literal extraction ──
    {
        auto lits = me_test::extractRegexLiterals("test.*\\.py");
        check(lits.size() >= 1, "60.1 extractRegexLiterals basic: got at least 1 literal");
        bool hasTest = false;
        for (auto& l : lits) {
            if (l == "test") hasTest = true;
        }
        check(hasTest, "60.1 extractRegexLiterals: extracted 'test'");
    }

    // ── Test 2: Escaped metachar becomes literal ──
    {
        auto lits = me_test::extractRegexLiterals("foo\\.bar");
        bool hasFoo = false, hasBar = false;
        for (auto& l : lits) {
            if (l == "foo") hasFoo = true;
            // "foo.bar" might be a single literal since \. is literal '.'
            if (l == "foo.bar") { hasFoo = true; hasBar = true; }
            if (l == "bar") hasBar = true;
        }
        check(hasFoo, "60.2 extractRegexLiterals escaped dot: got 'foo'");
    }

    // ── Test 3: \d shorthand breaks literal ──
    {
        auto lits = me_test::extractRegexLiterals("\\d+_config");
        bool hasConfig = false;
        for (auto& l : lits) {
            if (l.find("config") != std::string::npos) hasConfig = true;
        }
        check(hasConfig, "60.3 extractRegexLiterals \\d breaks: got 'config' substring");
    }

    // ── Test 4: Character class breaks literal ──
    {
        auto lits = me_test::extractRegexLiterals("[abc]defghi");
        bool hasDef = false;
        for (auto& l : lits) {
            if (l.find("def") != std::string::npos) hasDef = true;
        }
        check(hasDef, "60.4 extractRegexLiterals char class: got literal after class");
    }

    // ── Test 5: Short literals (< 3 chars) are skipped ──
    {
        auto lits = me_test::extractRegexLiterals("ab.*cd");
        // "ab" and "cd" are both < 3 chars, should be empty
        check(lits.empty(), "60.5 extractRegexLiterals: short literals filtered out");
    }

    // ── Test 6: Multiple literals extracted ──
    {
        auto lits = me_test::extractRegexLiterals("hello.*world\\.txt");
        bool hasHello = false, hasWorld = false;
        for (auto& l : lits) {
            if (l == "hello") hasHello = true;
            // "world.txt" is possible since \. is literal
            if (l.find("world") != std::string::npos) hasWorld = true;
        }
        check(hasHello && hasWorld, "60.6 extractRegexLiterals multiple: got both literals");
    }

    // ── Test 7: Alternation breaks literal ──
    {
        auto lits = me_test::extractRegexLiterals("alpha|beta");
        // alternation '|' breaks — each side < 5 chars but >= 3
        bool hasAlpha = false, hasBeta = false;
        for (auto& l : lits) {
            if (l == "alpha") hasAlpha = true;
            if (l == "beta") hasBeta = true;
        }
        // Both should exist (both are >= 3 chars)
        check(hasAlpha && hasBeta, "60.7 extractRegexLiterals alternation: got both sides");
    }

    // ── Test 8: Case normalization ──
    {
        auto lits = me_test::extractRegexLiterals("MyFile\\.TXT");
        bool hasLower = false;
        for (auto& l : lits) {
            if (l == "myfile.txt" || l.find("myfile") != std::string::npos) hasLower = true;
        }
        check(hasLower, "60.8 extractRegexLiterals case: lowercased output");
    }

    // ── Test 9: Regex trigram query on engine — trigram path ──
    {
        SearchEngine engine;
        // Insert records with distinct names
        for (int i = 0; i < 100; i++) {
            FileRecord r;
            r.name = "generic_file_" + std::to_string(i) + ".dat";
            r.path = "/data";
            r.size = 100;
            r.modTime = 1000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        // Insert target files with regex-matchable names
        for (int i = 0; i < 5; i++) {
            FileRecord r;
            r.name = "testcase_" + std::to_string(i) + ".pyc";
            r.path = "/projects";
            r.size = 200;
            r.modTime = 2000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("regex:testcase.*\\.pyc", 100, true, timing);
        check(results.size() == 5, "60.9 regex trigram query: got 5 results");
        // With only 105 records, trigram might or might not be used depending on
        // threshold, but results must be correct either way
    }

    // ── Test 10: Regex trigram query — results correctness ──
    {
        SearchEngine engine;
        // Create enough records so trigram pre-filter is meaningful
        for (int i = 0; i < 500; i++) {
            FileRecord r;
            r.name = "randomdata_" + std::to_string(i) + ".log";
            r.path = "/logs";
            r.size = 50;
            r.modTime = 1000000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        FileRecord target;
        target.name = "configuration_backup.json";
        target.path = "/etc";
        target.size = 300;
        target.modTime = 2000000;
        target.type = 1;
        engine.updateByPath(target.path + "/" + target.name, std::move(target));
        QueryTimingInfo timing;
        auto results = engine.queryAdvanced("regex:config.*backup", 100, true, timing);
        check(results.size() == 1, "60.10 regex trigram correctness: found target");
    }

    // ── Test 11: Regex with no extractable literals falls back to linear ──
    {
        SearchEngine engine;
        for (int i = 0; i < 50; i++) {
            FileRecord r;
            r.name = "file_" + std::to_string(i);
            r.path = "/tmp";
            r.size = 10;
            r.modTime = 1000;
            r.type = 1;
            engine.updateByPath(r.path + "/" + r.name, std::move(r));
        }
        QueryTimingInfo timing;
        // Pattern with only metacharacters, no extractable literals
        auto results = engine.queryAdvanced("regex:.*", 100, true, timing);
        check(results.size() == 50, "60.11 regex no literals: linear fallback, all matched");
        check(timing.searchPath == "advanced-linear-gcd", "60.11 searchPath is advanced-linear-gcd");
    }

    // ── Test 12: Empty pattern ──
    {
        auto lits = me_test::extractRegexLiterals("");
        check(lits.empty(), "60.12 extractRegexLiterals empty: no literals");
    }

    std::cout << "\n";
}
