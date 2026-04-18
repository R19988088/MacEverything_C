#pragma once
// Part 8: Trigram Index for Filename Search

static void runTrigramIndexTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 8: Trigram Index for Filename Search\n";
    std::cout << "========================================\n\n";

    // -- Test 1: Basic trigram-accelerated search --
    std::cout << "  --- Basic trigram search ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"document.pdf", "/home/user", 1, 100, 1000});
        records.push_back({"readme.md", "/home/user", 1, 200, 2000});
        records.push_back({"config.json", "/etc", 1, 300, 3000});
        records.push_back({"dockerfile", "/app", 1, 400, 4000});
        records.push_back({"doc_helper.py", "/src", 1, 500, 5000});
        engine.loadRecords(std::move(records));

        // "doc" has 1 trigram (d,o,c) — should use trigram index
        auto res = engine.query("doc");
        check(res.size() == 3, "Trigram: 'doc' matches document.pdf, dockerfile, doc_helper.py");

        // "document" has 6 trigrams — strong filtering
        res = engine.query("document");
        check(res.size() == 1, "Trigram: 'document' matches 1 file");
        check(engine.getRecord(res[0]).name == "document.pdf", "Trigram: 'document' matches document.pdf");

        // "readme" — unique match
        res = engine.query("readme");
        check(res.size() == 1, "Trigram: 'readme' matches 1 file");

        // "config" — unique match
        res = engine.query("config");
        check(res.size() == 1, "Trigram: 'config' matches 1 file");
    }

    // -- Test 2: Trigram index maintained through mutations --
    std::cout << "\n  --- Trigram index with mutations ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"alpha.txt", "/tmp", 1, 100, 1000});
        records.push_back({"beta.txt", "/tmp", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // Add a new record
        engine.addRecord({"alpha_v2.txt", "/tmp", 1, 300, 3000});
        auto res = engine.query("alpha");
        check(res.size() == 2, "Trigram add: 'alpha' matches 2 after addRecord");

        // Remove original alpha
        engine.removeByPath("/tmp/alpha.txt");
        res = engine.query("alpha");
        check(res.size() == 1, "Trigram remove: 'alpha' matches 1 after removeByPath");
        check(engine.getRecord(res[0]).name == "alpha_v2.txt", "Trigram remove: correct file remains");

        // Update beta to gamma
        engine.updateByPath("/tmp/beta.txt", {"gamma.txt", "/tmp", 1, 200, 2000});
        res = engine.query("beta");
        check(res.empty(), "Trigram update: 'beta' no longer matches after update");
        res = engine.query("gamma");
        check(res.size() == 1, "Trigram update: 'gamma' matches after update");
    }

    // -- Test 3: Trigram index survives compaction --
    std::cout << "\n  --- Trigram index after compaction ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        for (int i = 0; i < 20; i++) {
            records.push_back({"trigram_test_" + std::to_string(i) + ".txt", "/data", 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Remove half
        for (int i = 0; i < 10; i++) {
            engine.removeByPath("/data/trigram_test_" + std::to_string(i) + ".txt");
        }

        // Compact
        engine.compactRecords();
        check(engine.liveRecordCount() == 10, "Trigram compact: 10 live records after compaction");

        // Query should still work via trigram index
        auto res = engine.query("trigram_test_15");
        check(res.size() == 1, "Trigram compact: specific file still findable after compaction");

        auto res2 = engine.query("trigram_test_5");
        check(res2.empty(), "Trigram compact: removed file not findable after compaction");
    }

    // -- Test 4: Short keywords fallback to linear scan --
    std::cout << "\n  --- Short keyword fallback ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"ab.txt", "/tmp", 1, 100, 1000});
        records.push_back({"abc.txt", "/tmp", 1, 200, 2000});
        records.push_back({"abcd.txt", "/tmp", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // 2-char keyword: must fallback to linear scan
        auto res = engine.query("ab");
        check(res.size() == 3, "Short keyword 'ab': all 3 files match (linear scan fallback)");

        // Single char
        res = engine.query("a");
        check(res.size() == 3, "Single char 'a': all 3 files match");
    }

    // -- Test 5: Glob patterns bypass trigram index --
    std::cout << "\n  --- Glob pattern bypass ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"hello.cpp", "/src", 1, 100, 1000});
        records.push_back({"hello.h", "/src", 1, 50, 2000});
        records.push_back({"world.cpp", "/src", 1, 200, 3000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("*.cpp");
        check(res.size() == 2, "Glob '*.cpp': 2 matches (bypasses trigram)");

        res = engine.query("hello.*");
        check(res.size() == 2, "Glob 'hello.*': 2 matches (bypasses trigram)");
    }

    // -- Test 6: No-match keyword returns empty --
    std::cout << "\n  --- No-match via trigram ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"myfile.txt", "/tmp", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("xyz_nowhere");
        check(res.empty(), "No-match 'xyz_nowhere': trigram index returns empty");
    }

    // -- Test 7: Trigram search performance comparison --
    std::cout << "\n  --- Trigram search performance ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Build 100k records with varied names
        for (uint32_t i = 0; i < 100000; i++) {
            std::string name = "file_" + std::to_string(i) + "_data.txt";
            records.push_back({name, "/bench/dir" + std::to_string(i % 100), 1, i * 10, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Search for a very specific string with maxResults=100 (typical UI usage)
        auto t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("file_99999_data", 100);
            (void)res;
        }
        auto t1 = std::chrono::steady_clock::now();
        double trigramTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        // Same search but with a 2-char keyword (linear scan fallback)
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi", 100);  // 2 chars, linear scan
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double linearTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        // Also test trigram search without maxResults limit (worst case)
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("file_99999_data");
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double trigramUnlimitedTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "    100x trigram search (maxResults=100): " << std::fixed << std::setprecision(2)
                  << trigramTime << "ms (" << trigramTime / 100 << "ms/query)\n";
        std::cout << "    100x linear search (short key, maxResults=100): " << std::fixed << std::setprecision(2)
                  << linearTime << "ms (" << linearTime / 100 << "ms/query)\n";
        std::cout << "    100x trigram search (unlimited): " << std::fixed << std::setprecision(2)
                  << trigramUnlimitedTime << "ms (" << trigramUnlimitedTime / 100 << "ms/query)\n";

        if (trigramTime < linearTime) {
            double speedup = linearTime / trigramTime;
            std::cout << "    Trigram speedup (limited): " << std::fixed << std::setprecision(1) << speedup << "x\n";
        }
        check(true, "Trigram performance benchmark completed");

        // partial_sort benchmark: compare maxResults=100 vs unlimited (full sort)
        // With partial_sort, limited queries should be faster on large result sets
        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi", 100);  // many matches, partial_sort
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double partialSortTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        t0 = std::chrono::steady_clock::now();
        for (int run = 0; run < 100; run++) {
            auto res = engine.query("fi");  // many matches, full sort
            (void)res;
        }
        t1 = std::chrono::steady_clock::now();
        double fullSortTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "    100x partial_sort (maxResults=100, many matches): " << std::fixed << std::setprecision(2)
                  << partialSortTime << "ms (" << partialSortTime / 100 << "ms/query)\n";
        std::cout << "    100x full_sort (unlimited, many matches): " << std::fixed << std::setprecision(2)
                  << fullSortTime << "ms (" << fullSortTime / 100 << "ms/query)\n";
        if (partialSortTime < fullSortTime) {
            double speedup = fullSortTime / partialSortTime;
            std::cout << "    partial_sort speedup: " << std::fixed << std::setprecision(1) << speedup << "x\n";
        }
        check(true, "partial_sort benchmark completed");
    }

    // -- Test 8: Path-only match still works with trigram --
    std::cout << "\n  --- Path-only match with trigram ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
        records.push_back({"readme.md", "/usr/local/docs", 1, 200, 2000});
        records.push_back({"local.conf", "/etc", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "local" matches local.conf by name, and both others by path
        auto res = engine.query("local");
        check(res.size() == 3, "Path+Trigram: 'local' matches 3 (1 name + 2 path)");

        // Name match should come before path matches
        check(engine.getRecord(res[0]).name == "local.conf",
              "Path+Trigram: name match 'local.conf' ranked first");
    }

    // -- Test 9: removeByPathPrefix with trigram cleanup --
    std::cout << "\n  --- removeByPathPrefix with trigram ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"aaa.txt", "/prefix/sub", 1, 100, 1000});
        records.push_back({"bbb.txt", "/prefix/sub", 1, 200, 2000});
        records.push_back({"ccc.txt", "/other", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        engine.removeByPathPrefix("/prefix");
        auto res = engine.query("aaa");
        check(res.empty(), "removeByPathPrefix: 'aaa' no longer findable via trigram");
        res = engine.query("ccc");
        check(res.size() == 1, "removeByPathPrefix: 'ccc' still findable via trigram");
    }

    // -- Test 10: removeByPathPrefix benchmark (single-pass vs old double-lookup) --
    std::cout << "\n  --- removeByPathPrefix benchmark ---\n";
    {
        // Build a large dataset: 50K records under 10 different prefixes
        const int recordsPerPrefix = 5000;
        const int numPrefixes = 10;
        const int totalRecords = recordsPerPrefix * numPrefixes;

        auto buildRecords = [&]() {
            std::vector<FileRecord> records;
            records.reserve(totalRecords);
            for (int p = 0; p < numPrefixes; p++) {
                std::string prefix = "/volume/prefix" + std::to_string(p);
                for (int i = 0; i < recordsPerPrefix; i++) {
                    std::string name = "file_" + std::to_string(p) + "_" + std::to_string(i) + ".txt";
                    records.push_back({name, prefix + "/sub/deep", 1, static_cast<uint64_t>(i * 100), static_cast<time_t>(1000 + i)});
                }
            }
            return records;
        };

        // Benchmark: remove one prefix (5000 records out of 50000)
        SearchEngine engine;
        engine.loadRecords(buildRecords());
        auto t0 = std::chrono::steady_clock::now();
        uint32_t removed = engine.removeByPathPrefix("/volume/prefix0");
        auto t1 = std::chrono::steady_clock::now();
        double removeTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        check(removed == recordsPerPrefix, "removeByPathPrefix: removed correct count");
        check(engine.liveRecordCount() == totalRecords - recordsPerPrefix,
              "removeByPathPrefix: live count correct after removal");

        // Verify removed records are not queryable
        auto res = engine.query("file_0_0");
        check(res.empty(), "removeByPathPrefix: removed file not queryable");
        // Verify non-removed records still queryable
        res = engine.query("file_1_0");
        check(res.size() >= 1, "removeByPathPrefix: non-removed file still queryable");

        std::cout << "    Remove " << removed << " / " << totalRecords << " records: "
                  << std::fixed << std::setprecision(2) << removeTime << "ms\n";
        check(true, "removeByPathPrefix benchmark completed");
    }

    // -- Test 11: Trigram candidate threshold fallback to linear scan --
    std::cout << "\n  --- Trigram candidate threshold fallback ---\n";
    {
        // Create a dataset where a common keyword produces candidates > 1.5% of total
        // Total = 10000 records, threshold = 10000/67 ≈ 149
        // "test" appears in 200 filenames (2% > 1.5%) → should fallback to linear
        // "unique_xyz" appears in 5 filenames (0.05% < 1.5%) → should stay trigram
        const uint32_t totalRecords = 10000;
        const uint32_t commonCount = 200; // 2% of total

        SearchEngine engine;
        std::vector<FileRecord> records;
        records.reserve(totalRecords);

        // First commonCount records contain "test" in the name
        for (uint32_t i = 0; i < commonCount; i++) {
            records.push_back({"test_file_" + std::to_string(i) + ".txt",
                              "/data/dir" + std::to_string(i % 50), 1, (uint64_t)i, (time_t)i});
        }
        // A few records with a rare keyword
        for (uint32_t i = 0; i < 5; i++) {
            records.push_back({"unique_xyz_" + std::to_string(i) + ".txt",
                              "/data/rare", 1, (uint64_t)(commonCount + i), (time_t)(commonCount + i)});
        }
        // Fill remaining with unrelated names
        for (uint32_t i = commonCount + 5; i < totalRecords; i++) {
            records.push_back({"item_" + std::to_string(i) + ".dat",
                              "/data/bulk" + std::to_string(i % 100), 1, (uint64_t)i, (time_t)i});
        }
        engine.loadRecords(std::move(records));

        // Common keyword "test" should exceed threshold and fallback to linear
        QueryTimingInfo timing1;
        auto res1 = engine.query("test", 0, true, timing1);
        check(res1.size() == commonCount,
              "Threshold fallback: 'test' finds all matches");
        check(timing1.searchPath == "linear",
              "Threshold fallback: 'test' uses linear scan");

        // Rare keyword "unique_xyz" should stay on trigram path
        QueryTimingInfo timing2;
        auto res2 = engine.query("unique_xyz", 0, true, timing2);
        check(res2.size() == 5, "Threshold fallback: 'unique_xyz' finds 5 matches");
        check(timing2.searchPath != "linear",
              "Threshold fallback: 'unique_xyz' stays on trigram");

        std::cout << "    'test': " << res1.size() << " results, path=" << timing1.searchPath
                  << ", candidates=" << timing1.candidates << "\n";
        std::cout << "    'unique_xyz': " << res2.size() << " results, path=" << timing2.searchPath
                  << ", candidates=" << timing2.candidates << "\n";
    }

    std::cout << "\n";
}
