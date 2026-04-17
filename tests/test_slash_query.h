#pragma once
// Part 48: Slash Query Trigram Split

static void runSlashQueryTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 48: Slash Query Trigram Split\n";
    std::cout << "========================================\n\n";

    // -- Test 1: Basic slash query --
    std::cout << "  --- Test 1: Basic slash query ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file.txt", "/path/to/dir", 1, 100, 1000});
        records.push_back({"other.txt", "/path/to/dir", 1, 200, 2000});
        records.push_back({"file.txt", "/unrelated/place", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "dir/file" should match only /path/to/dir/file.txt
        auto res = engine.query("dir/file");
        check(res.size() == 1, "Slash query: 'dir/file' matches 1 result");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "file.txt", "Slash query: matched file is file.txt");
    }

    // -- Test 2: Deep path slash query --
    std::cout << "\n  --- Test 2: Deep path slash query ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"gcc", "/usr/local/bin", 1, 100, 1000});
        records.push_back({"lib.a", "/usr/local/lib", 1, 200, 2000});
        records.push_back({"readme.txt", "/usr/share/doc", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "local/bin" should match only gcc in /usr/local/bin
        auto res = engine.query("local/bin");
        check(res.size() == 1, "Slash query: 'local/bin' matches 1 file");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "gcc", "Slash query: matched file is gcc");

        // "usr/local" should match both files under /usr/local/
        res = engine.query("usr/local");
        check(res.size() == 2, "Slash query: 'usr/local' matches 2 files");
    }

    // -- Test 3: Long name slash query (the original perf issue) --
    std::cout << "\n  --- Test 3: Long name slash query ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test_query_perf_10m.h", "/project/tests", 1, 100, 1000});
        records.push_back({"test_query_basic.h", "/project/tests", 1, 200, 2000});
        records.push_back({"test_query_perf_10m.h", "/other/tests", 1, 300, 3000});
        records.push_back({"unrelated.cpp", "/project/src", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        // "tests/test_query_perf" should match both test_query_perf_10m.h files
        auto res = engine.query("tests/test_query_perf");
        check(res.size() == 2, "Slash query: 'tests/test_query_perf' matches 2 results");
    }

    // -- Test 4: No match --
    std::cout << "\n  --- Test 4: Slash query no match ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file.txt", "/existing/path", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("nonexist/dir");
        check(res.size() == 0, "Slash query: 'nonexist/dir' returns empty");
    }

    // -- Test 5: Short parts fallback to linear scan --
    std::cout << "\n  --- Test 5: Short parts fallback ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"b.c", "/a", 1, 100, 1000});
        records.push_back({"d.c", "/a", 1, 200, 2000});
        records.push_back({"b.c", "/x", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "a/b" — both parts < 3 chars, should fall back to linear scan
        // but still produce correct results
        auto res = engine.query("a/b");
        check(res.size() == 1, "Slash query: 'a/b' with short parts still finds correct result");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "b.c", "Slash query: short parts matched b.c");
    }

    // -- Test 6: Phase 1 interaction (no double-counting) --
    std::cout << "\n  --- Test 6: Phase 1 no double-counting ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // A file whose name contains "tests" — Phase 1 might match on name "tests_helper.cpp"
        // but the slash query "src/tests" should also find it via Phase 2
        records.push_back({"tests_helper.cpp", "/project/src", 1, 100, 1000});
        records.push_back({"tests_helper.cpp", "/project/lib", 1, 200, 2000});
        records.push_back({"main.cpp", "/project/src", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "src/tests" should match only the one in /project/src, not /project/lib
        auto res = engine.query("src/tests");
        check(res.size() == 1, "Slash query: 'src/tests' matches only the file under /project/src");
        auto rec = engine.getRecord(res[0]);
        check(std::string(rec.name) == "tests_helper.cpp", "Slash query: correct file matched");

        // Verify no duplicates in results
        // Query without slash for comparison
        auto res2 = engine.query("tests_helper");
        check(res2.size() == 2, "Non-slash query: 'tests_helper' finds both files");
    }
}
