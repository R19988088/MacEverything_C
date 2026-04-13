// ═══════════════════════════════════════════════════════
//  Part 9 — Phase 3 Regression Tests (Bridge layer C++ aspects)
// ═══════════════════════════════════════════════════════

void runPhase3Tests() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << " Part 9: Phase 3 — Bridge Layer Regression\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    // B4: recentIndices batch method
    {
        std::cout << "  [B4] recentIndices batch method ... ";
        SearchEngine engine;
        std::vector<FileRecord> records;

        // Create records with different modTimes
        for (int i = 0; i < 100; i++) {
            FileRecord r;
            r.name = "file" + std::to_string(i) + ".txt";
            r.path = "/tmp/test";
            r.type = 1;
            r.size = 100;
            r.modTime = static_cast<time_t>(1000 + i); // increasingly recent
            r.inode = static_cast<uint64_t>(i + 1);
            r.devId = 1;
            records.push_back(std::move(r));
        }
        engine.loadRecords(std::move(records));

        auto top5 = engine.recentIndices(5);
        check(top5.size() == 5, "recentIndices returns 5 results");

        // Top 5 should be indices 99, 98, 97, 96, 95 (most recent modTimes)
        check(top5[0] == 99, "recentIndices[0] is most recent");
        check(top5[1] == 98, "recentIndices[1] is second most recent");
        check(top5[4] == 95, "recentIndices[4] is fifth most recent");

        // Request more than available
        auto all = engine.recentIndices(200);
        check(all.size() == 100, "recentIndices clamps to record count");

        // Request 0
        auto none = engine.recentIndices(0);
        check(none.empty(), "recentIndices(0) returns empty");

        std::cout << "OK\n";
    }

    // B4: recentIndices skips tombstoned records
    {
        std::cout << "  [B4] recentIndices skips tombstones ... ";
        SearchEngine engine;
        std::vector<FileRecord> records;

        for (int i = 0; i < 10; i++) {
            FileRecord r;
            r.name = "file" + std::to_string(i) + ".txt";
            r.path = "/tmp/test";
            r.type = 1;
            r.size = 100;
            r.modTime = static_cast<time_t>(1000 + i);
            r.inode = static_cast<uint64_t>(i + 1);
            r.devId = 1;
            records.push_back(std::move(r));
        }
        engine.loadRecords(std::move(records));

        // Remove the most recent record (index 9)
        engine.removeByPath("/tmp/test/file9.txt");

        auto top3 = engine.recentIndices(3);
        check(top3.size() == 3, "recentIndices returns 3 after removal");
        check(top3[0] == 8, "most recent after removal is index 8");
        check(top3[1] == 7, "second most recent after removal is index 7");

        std::cout << "OK\n";
    }

    // B7: content index removal ordering — indexForPath must work before removeByPath
    {
        std::cout << "  [B7] content index removal ordering ... ";
        SearchEngine engine;
        auto contentIndex = std::make_shared<ContentIndex>();

        FileRecord r;
        r.name = "test.cpp";
        r.path = "/tmp/b7test";
        r.type = 1;
        r.size = 50;
        r.modTime = time(nullptr);
        r.inode = 42;
        r.devId = 1;
        uint32_t idx = engine.addRecord(std::move(r));

        // Index content for this file
        std::vector<Trigram> trigrams = {100, 200, 300};
        contentIndex->insertFileInfo(idx, 0xABCD, std::move(trigrams));
        check(contentIndex->indexedFileCount() == 1, "content index has 1 file");

        // Simulate correct ordering: look up index BEFORE removing from engine
        std::string fullPath = "/tmp/b7test/test.cpp";
        uint32_t lookupIdx = engine.indexForPath(fullPath);
        check(lookupIdx == idx, "indexForPath finds record before removal");

        // Remove from content index using the looked-up index
        contentIndex->removeFile(lookupIdx);
        check(contentIndex->indexedFileCount() == 0, "content index empty after removal");

        // Now remove from engine
        bool removed = engine.removeByPath(fullPath);
        check(removed, "removeByPath succeeds");

        // After engine removal, indexForPath should return UINT32_MAX
        uint32_t afterIdx = engine.indexForPath(fullPath);
        check(afterIdx == UINT32_MAX, "indexForPath returns UINT32_MAX after removal");

        std::cout << "OK\n";
    }

    // B3: saveToFile with metadata preserves fields
    {
        std::cout << "  [B3] saveToFile preserves metadata ... ";
        SearchEngine engine;

        FileRecord r;
        r.name = "meta_test.txt";
        r.path = "/tmp/b3test";
        r.type = 1;
        r.size = 123;
        r.modTime = time(nullptr);
        r.inode = 99;
        r.devId = 2;
        engine.addRecord(std::move(r));

        std::string tmpFile = "/tmp/test_phase3_metadata.bin";

        IndexMetadata metaOut;
        metaOut.lastEventId = 12345;
        metaOut.extra["app_version"] = "1.1.0";
        metaOut.extra["os_version"] = "14.5.0";
        metaOut.extra["record_format"] = "v3_inode";

        bool saved = engine.saveToFile(tmpFile, metaOut);
        check(saved, "saveToFile with metadata succeeds");

        SearchEngine engine2;
        IndexMetadata metaIn;
        bool loaded = engine2.loadFromFile(tmpFile, &metaIn);
        check(loaded, "loadFromFile succeeds");
        check(metaIn.lastEventId == 12345, "lastEventId preserved");
        check(metaIn.formatVersion == 3, "format version is v3");
        check(metaIn.extra["app_version"] == "1.1.0", "app_version metadata preserved");
        check(metaIn.extra["os_version"] == "14.5.0", "os_version metadata preserved");
        check(metaIn.extra["record_format"] == "v3_inode", "record_format metadata preserved");
        check(engine2.liveRecordCount() == 1, "record count preserved");

        remove(tmpFile.c_str());
        std::cout << "OK\n";
    }

    std::cout << "\n";
}
