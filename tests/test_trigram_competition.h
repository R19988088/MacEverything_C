#pragma once
#include <cassert>
#include <iostream>
#include <string>
#include "../MacEverything/Core/SearchEngine.h"
#include "../MacEverything/Core/CompiledGlob.h"

// ── Part 69: Trigram Competition Tests ──
// Verifies that GLOB patterns can participate in trigram pre-filtering
// and that name vs path trigram selection picks the better anchor.

/// Helper: add N dummy records with distinct names to inflate totalSize so that
/// the trigram threshold (totalSize/67) permits pre-filtering.
static void addDummyRecords(std::vector<FileRecord>& records, int n,
                            const std::string& prefix = "dummy",
                            const std::string& path = "/noise") {
    for (int i = 0; i < n; i++) {
        FileRecord r;
        r.name = prefix + std::to_string(i) + ".dat";
        r.path = path;
        r.type = 1;
        records.push_back(std::move(r));
    }
}

static void runTrigramCompetitionTests() {
    std::cout << "── Part 69: Trigram Competition ──\n";
    int localPassed = 0, localFailed = 0;

    auto check = [&](bool cond, const std::string& msg) {
        if (cond) { localPassed++; passed++; std::cout << "    [PASS] " << msg << "\n"; }
        else { localFailed++; failed++; std::cout << "    [FAIL] " << msg << "\n"; }
    };

    // ── 68.1 GLOB trigram: *.txt uses trigram, not linear-gcd ──
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        FileRecord r1; r1.name = "readme.txt"; r1.path = "/tmp"; r1.type = 1;
        FileRecord r2; r2.name = "notes.txt"; r2.path = "/tmp"; r2.type = 1;
        FileRecord r3; r3.name = "data.txt"; r3.path = "/tmp"; r3.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        records.push_back(std::move(r3));
        // Add 200 dummy records (none with ".txt") so trigram threshold works
        addDummyRecords(records, 200);
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto results = engine.query("*.txt", 100, true, timing);
        check(results.size() == 3, "69.1 *.txt finds 3 .txt files");
        check(timing.searchPath.find("trigram") != std::string::npos,
              "69.1 *.txt uses trigram path (got: " + timing.searchPath + ")");
    }

    // ── 68.2 GLOB trigram: *.cpp uses trigram with correct candidates ──
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        FileRecord r1; r1.name = "main.cpp"; r1.path = "/src"; r1.type = 1;
        FileRecord r2; r2.name = "util.cpp"; r2.path = "/src"; r2.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        addDummyRecords(records, 200);
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto results = engine.query("*.cpp", 100, true, timing);
        check(results.size() == 2, "69.2 *.cpp finds 2 files");
        check(timing.candidates > 0 && timing.candidates < timing.totalRecords,
              "69.2 trigram narrows candidates (cands=" + std::to_string(timing.candidates)
              + " total=" + std::to_string(timing.totalRecords) + ")");
    }

    // ── 68.3 Path trigram competition: path with fewer candidates wins ──
    // Uses "/raresegment/common" syntax which triggers transformSlashTerms → __pathseg
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // "common" appears in many names → high name trigram candidates
        for (int i = 0; i < 100; i++) {
            FileRecord r;
            r.name = "common_file_" + std::to_string(i) + ".txt";
            r.path = "/frequentpath";
            r.type = 1;
            records.push_back(std::move(r));
        }
        // Only 2 records under /raresegment — path trigram for "raresegment" is very selective
        FileRecord r1; r1.name = "common_file_special.txt"; r1.path = "/raresegment"; r1.type = 1;
        FileRecord r2; r2.name = "common_file_other.txt"; r2.path = "/raresegment"; r2.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        // Add noise to make path trigram ratio work
        addDummyRecords(records, 100, "unrelated", "/otherpath");
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        // "/raresegment/common" → __pathseg(raresegment) + TERM(common)
        auto results = engine.query("/raresegment/common", 100, true, timing);
        check(results.size() == 2, "69.3 path competition: 2 results");
        // With competition, path trigram (2 records) should beat name trigram (102 records)
        check(timing.searchPath == "advanced-path-trigram",
              "69.3 path-trigram wins competition (got: " + timing.searchPath + ")");
    }

    // ── 68.4 Pure name query unaffected: "main" still uses advanced-trigram ──
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        FileRecord r1; r1.name = "main.cpp"; r1.path = "/src"; r1.type = 1;
        FileRecord r2; r2.name = "main.h"; r2.path = "/src"; r2.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        addDummyRecords(records, 200);
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto results = engine.query("main", 100, true, timing);
        check(results.size() == 2, "69.4 'main' finds 2 files");
        check(timing.searchPath == "advanced-trigram",
              "69.4 pure name uses advanced-trigram (got: " + timing.searchPath + ")");
    }

    // ── 68.5 Short glob literal: *a* has literal "a" (1 char < 3), falls to linear ──
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        FileRecord r1; r1.name = "abc.txt"; r1.path = "/tmp"; r1.type = 1;
        FileRecord r2; r2.name = "xyz.txt"; r2.path = "/tmp"; r2.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        addDummyRecords(records, 200);
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto results = engine.query("*a*", 100, true, timing);
        check(timing.searchPath == "advanced-linear-gcd",
              "69.5 *a* falls to linear (got: " + timing.searchPath + ")");
    }

    // ── 68.6 GLOB with long literal: test*.swift uses "test" or "swift" for trigram ──
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        FileRecord r1; r1.name = "test_main.swift"; r1.path = "/src"; r1.type = 1;
        FileRecord r2; r2.name = "test_util.swift"; r2.path = "/src"; r2.type = 1;
        records.push_back(std::move(r1));
        records.push_back(std::move(r2));
        addDummyRecords(records, 200);
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto results = engine.query("test*.swift", 100, true, timing);
        check(results.size() == 2, "69.6 test*.swift finds 2 files");
        check(timing.searchPath.find("trigram") != std::string::npos,
              "69.6 test*.swift uses trigram (got: " + timing.searchPath + ")");
    }

    std::cout << "  Part 69 summary: " << localPassed << " passed, " << localFailed << " failed\n";
}
