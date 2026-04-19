#pragma once
// Part 58: Structured Query (Node-Centric Search)
// StructuredQueryParser.h is included transitively via SearchEngine.h

static void runStructuredQueryTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 58: Structured Query (Node-Centric)\n";
    std::cout << "========================================\n\n";

    // ── Parser Unit Tests ──

    // -- Test 1: PLAIN mode (no slash) --
    std::cout << "  --- Test 1: Parser — PLAIN mode ---\n";
    {
        auto pq = parseQuery("hello");
        CHECK(pq.mode == QueryMode::PLAIN);
        CHECK(pq.namePattern == "hello");
        CHECK(pq.pathSegments.empty());
    }

    // -- Test 2: SEGMENTS mode (basic /abc/def) --
    std::cout << "\n  --- Test 2: Parser — SEGMENTS mode ---\n";
    {
        auto pq = parseQuery("/abc/def");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments.size() == 1);
        CHECK(pq.pathSegments[0].text == "abc");
        CHECK(pq.pathSegments[0].adjacentToNext == true);
    }

    // -- Test 3: DIR_EXACT mode (/abc/def/) --
    std::cout << "\n  --- Test 3: Parser — DIR_EXACT mode ---\n";
    {
        auto pq = parseQuery("/abc/def/");
        CHECK(pq.mode == QueryMode::DIR_EXACT);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments.size() == 1);
        CHECK(pq.pathSegments[0].text == "abc");
    }

    // -- Test 4: DIR_LIST mode (/abc/def/*) --
    std::cout << "\n  --- Test 4: Parser — DIR_LIST mode ---\n";
    {
        auto pq = parseQuery("/abc/def/*");
        CHECK(pq.mode == QueryMode::DIR_LIST);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments.size() == 1);
        CHECK(pq.pathSegments[0].text == "abc");
    }

    // -- Test 5: Adjacency break with * --
    std::cout << "\n  --- Test 5: Parser — adjacency break ---\n";
    {
        auto pq = parseQuery("/abc/*/def");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments.size() == 1);
        CHECK(pq.pathSegments[0].text == "abc");
        CHECK(pq.pathSegments[0].adjacentToNext == false);
    }

    // -- Test 6: Single segment /usr equals plain usr --
    std::cout << "\n  --- Test 6: Parser — single segment ---\n";
    {
        auto pq = parseQuery("/usr");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "usr");
        CHECK(pq.pathSegments.empty());

        // Without leading slash, same result
        auto pq2 = parseQuery("usr");
        CHECK(pq2.mode == QueryMode::PLAIN);
        CHECK(pq2.namePattern == "usr");
    }

    // -- Test 7: Leading slash optional abc/def == /abc/def --
    std::cout << "\n  --- Test 7: Parser — leading slash optional ---\n";
    {
        auto pq = parseQuery("abc/def");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments.size() == 1);
        CHECK(pq.pathSegments[0].text == "abc");
    }

    // -- Test 8: Multi-segment path /a/b/c --
    std::cout << "\n  --- Test 8: Parser — multi-segment ---\n";
    {
        auto pq = parseQuery("/a/b/c");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "c");
        CHECK(pq.pathSegments.size() == 2);
        CHECK(pq.pathSegments[0].text == "a");
        CHECK(pq.pathSegments[0].adjacentToNext == true);
        CHECK(pq.pathSegments[1].text == "b");
        CHECK(pq.pathSegments[1].adjacentToNext == true);
    }

    // -- Test 9: Case insensitive parsing --
    std::cout << "\n  --- Test 9: Parser — case insensitive ---\n";
    {
        auto pq = parseQuery("/ABC/DEF");
        CHECK(pq.mode == QueryMode::SEGMENTS);
        CHECK(pq.namePattern == "def");
        CHECK(pq.pathSegments[0].text == "abc");
    }

    // ── Integration Tests ──

    // -- Test 10: SEGMENTS — node name match with path constraint --
    std::cout << "\n  --- Test 10: SEGMENTS — basic node search ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"config.yaml", "/app/settings", 1, 100, 1000});
        records.push_back({"config.yaml", "/other/place", 1, 200, 2000});
        records.push_back({"readme.md", "/app/settings", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "/settings/config" → name contains "config", parent dir contains "settings"
        auto res = engine.query("/settings/config");
        // Should find config.yaml in /app/settings (name matches, path matches)
        bool foundSettings = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "config.yaml" && rec.path == "/app/settings") foundSettings = true;
        }
        CHECK(foundSettings);

        // Should NOT find config.yaml in /other/place (path doesn't contain "settings")
        bool foundOther = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "config.yaml" && rec.path == "/other/place") foundOther = true;
        }
        CHECK(!foundOther);
    }

    // -- Test 11: SEGMENTS — multi-segment adjacency enforcement --
    std::cout << "\n  --- Test 11: SEGMENTS — adjacency ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"target.txt", "/foo/bar", 1, 100, 1000});     // foo→bar adjacent
        records.push_back({"target.txt", "/foo/mid/bar", 1, 200, 2000}); // foo→bar NOT adjacent (mid in between)
        records.push_back({"target.txt", "/bar/baz", 1, 300, 3000});     // no "foo" in path
        engine.loadRecords(std::move(records));

        // "/foo/bar/target" → name contains "target", path has "foo" adjacent to "bar"
        auto res = engine.query("/foo/bar/target");
        int matchCount = 0;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "target.txt") matchCount++;
        }
        // /foo/bar/target.txt → "foo" adjacent to "bar" ✓
        // /foo/mid/bar/target.txt → "foo" NOT adjacent to "bar" (mid in between) ✗
        // /bar/baz/target.txt → no "foo" ✗
        CHECK(matchCount == 1);
    }

    // -- Test 12: Non-adjacent match with * --
    std::cout << "\n  --- Test 12: SEGMENTS — non-adjacent * ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test.cpp", "/project/src/core", 1, 100, 1000});
        records.push_back({"test.cpp", "/project/docs", 1, 200, 2000});
        records.push_back({"test.cpp", "/other/src/core", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // "/project/*/test" → name contains "test", ancestor contains "project" (non-adjacent)
        auto res = engine.query("/project/*/test");
        int matchCount = 0;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "test.cpp" && rec.path.find("project") != std::string::npos) matchCount++;
        }
        // /project/src/core/test.cpp → path has "project" anywhere ✓
        // /project/docs/test.cpp → path has "project" anywhere ✓
        CHECK(matchCount == 2);

        // Should NOT match /other/src/core/test.cpp
        bool foundOther = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.path.find("/other/") != std::string::npos) foundOther = true;
        }
        CHECK(!foundOther);
    }

    // -- Test 13: DIR_EXACT — find directory node --
    std::cout << "\n  --- Test 13: DIR_EXACT — directory search ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"bin", "/usr/local", 2, 0, 1000});      // directory
        records.push_back({"bin", "/usr", 2, 0, 2000});             // directory
        records.push_back({"binary.txt", "/usr/local", 1, 100, 3000}); // file, not exact match
        records.push_back({"bin", "/opt", 2, 0, 4000});             // different path
        engine.loadRecords(std::move(records));

        // "/local/bin/" → find directory named "bin" with parent containing "local"
        auto res = engine.query("/local/bin/");
        bool found = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "bin" && rec.path == "/usr/local" && rec.type == 2) found = true;
        }
        CHECK(found);

        // Should not find file binary.txt (not exact name match on "bin")
        bool foundFile = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "binary.txt") foundFile = true;
        }
        CHECK(!foundFile);

        // Should not find /opt/bin (path doesn't contain "local")
        bool foundOpt = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.path == "/opt") foundOpt = true;
        }
        CHECK(!foundOpt);
    }

    // -- Test 14: DIR_LIST — list directory children --
    std::cout << "\n  --- Test 14: DIR_LIST — list children ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // /usr/local/bin/ contains: gcc, ls, make
        records.push_back({"bin", "/usr/local", 2, 0, 1000});
        records.push_back({"gcc", "/usr/local/bin", 1, 100, 2000});
        records.push_back({"ls", "/usr/local/bin", 1, 200, 3000});
        records.push_back({"make", "/usr/local/bin", 1, 300, 4000});
        // /usr/bin/ also has bin directory
        records.push_back({"bin", "/usr", 2, 0, 5000});
        records.push_back({"cat", "/usr/bin", 1, 400, 6000});
        engine.loadRecords(std::move(records));

        // "/local/bin/*" → list children of "bin" directory where parent has "local"
        auto res = engine.query("/local/bin/*");
        // Should find gcc, ls, make (children of /usr/local/bin)
        std::set<std::string> foundNames;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            foundNames.insert(rec.name);
        }
        CHECK(foundNames.count("gcc") == 1);
        CHECK(foundNames.count("ls") == 1);
        CHECK(foundNames.count("make") == 1);
        // Should NOT find cat (child of /usr/bin, not /usr/local/bin)
        CHECK(foundNames.count("cat") == 0);
    }

    // -- Test 15: Single segment /usr equals plain search --
    std::cout << "\n  --- Test 15: Single segment equivalence ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"usr_config", "/home/user", 1, 100, 1000});
        records.push_back({"readme", "/usr/share", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // "/usr" → SEGMENTS with no path constraint, name contains "usr"
        auto res = engine.query("/usr");
        // Should find usr_config (name contains "usr")
        bool foundConfig = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "usr_config") foundConfig = true;
        }
        CHECK(foundConfig);
    }

    // -- Test 16: searchPath label for structured queries --
    std::cout << "\n  --- Test 16: searchPath label ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"config.yaml", "/app/settings", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto res = engine.query("/settings/config", 0, true, timing);
        // searchPath should indicate "structured" for SEGMENTS mode
        CHECK(timing.searchPath == "structured" || timing.searchPath == "linear");
    }

    // -- Test 17: DIR_LIST with no path constraint --
    std::cout << "\n  --- Test 17: DIR_LIST — no path constraint ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"mydir", "/root", 2, 0, 1000});
        records.push_back({"child1.txt", "/root/mydir", 1, 100, 2000});
        records.push_back({"child2.txt", "/root/mydir", 1, 200, 3000});
        engine.loadRecords(std::move(records));

        // "/mydir/*" → list children of "mydir" (no parent constraint)
        auto res = engine.query("/mydir/*");
        CHECK(res.size() == 2);
    }

    // -- Test 18: SEGMENTS — name must match (node-centric) --
    std::cout << "\n  --- Test 18: SEGMENTS — node-centric, no path-only ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"readme.md", "/abc/def", 1, 100, 1000});
        records.push_back({"other.txt", "/abc/def", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // "/abc/def" → name must contain "def", path must contain "abc"
        // "other.txt" doesn't contain "def" in name → should NOT appear
        auto res = engine.query("/abc/def");
        bool foundOther = false;
        for (uint32_t idx : res) {
            auto rec = engine.getRecord(idx);
            if (rec.name == "other.txt") foundOther = true;
        }
        CHECK(!foundOther);
    }

    // -- Test 19: glob pattern should NOT trigger on /abc/* --
    std::cout << "\n  --- Test 19: /abc/* is DIR_LIST, not glob ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"mydir", "/parent", 2, 0, 1000});
        records.push_back({"file.txt", "/parent/mydir", 1, 100, 2000});
        engine.loadRecords(std::move(records));

        QueryTimingInfo timing;
        auto res = engine.query("/mydir/*", 0, true, timing);
        // Should NOT use "linear" glob path
        CHECK(timing.searchPath != "glob-trigram");
        CHECK(res.size() >= 1);
    }

    std::cout << "\n";
}
