#pragma once
// ═══════════════════════════════════════════════════════
//  Phase 1: High-Severity Bug Regression Tests (C1-C5)
// ═══════════════════════════════════════════════════════

// --- C1: compactRecords() must return old→new index mapping ---
// Without a remap, ContentIndex fileIndices become stale after compaction.
static void runC1_CompactionRemapTest() {
    std::cout << "========================================\n";
    std::cout << "  C1: compactRecords returns index remap\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;
    std::vector<FileRecord> records;
    records.push_back({"file_0.txt", "/test", 1, 100, 1000});  // index 0
    records.push_back({"file_1.txt", "/test", 1, 200, 2000});  // index 1
    records.push_back({"file_2.txt", "/test", 1, 300, 3000});  // index 2
    records.push_back({"file_3.txt", "/test", 1, 400, 4000});  // index 3
    records.push_back({"file_4.txt", "/test", 1, 500, 5000});  // index 4
    engine.loadRecords(std::move(records));

    // Remove records 0 and 2 — creates gaps
    engine.removeByPath("/test/file_0.txt");
    engine.removeByPath("/test/file_2.txt");

    check(engine.liveRecordCount() == 3, "C1: 3 live records before compaction");

    // Compact and get remap
    auto remap = engine.compactRecords();

    check(engine.recordCount() == 3, "C1: recordCount == 3 after compaction");
    check(engine.liveRecordCount() == 3, "C1: liveRecordCount == 3 after compaction");

    // Verify remap is returned and correct
    check(!remap.empty(), "C1: remap is not empty");

    // Old index 1 → should map to new index 0
    // Old index 3 → should map to new index 1
    // Old index 4 → should map to new index 2
    if (remap.count(1)) {
        check(remap[1] == 0, "C1: old index 1 → new index 0");
    } else {
        check(false, "C1: remap contains old index 1");
    }
    if (remap.count(3)) {
        check(remap[3] == 1, "C1: old index 3 → new index 1");
    } else {
        check(false, "C1: remap contains old index 3");
    }
    if (remap.count(4)) {
        check(remap[4] == 2, "C1: old index 4 → new index 2");
    } else {
        check(false, "C1: remap contains old index 4");
    }

    // Tombstoned indices (0, 2) should not be in the remap
    check(remap.count(0) == 0, "C1: tombstoned index 0 not in remap");
    check(remap.count(2) == 0, "C1: tombstoned index 2 not in remap");

    // Verify records are still queryable with new indices
    auto res = engine.query("file_1");
    check(res.size() == 1 && res[0] == 0, "C1: file_1.txt queryable at new index 0");

    res = engine.query("file_3");
    check(res.size() == 1 && res[0] == 1, "C1: file_3.txt queryable at new index 1");

    res = engine.query("file_4");
    check(res.size() == 1 && res[0] == 2, "C1: file_4.txt queryable at new index 2");

    // No-op compaction returns empty remap
    auto remap2 = engine.compactRecords();
    check(remap2.empty(), "C1: no-op compaction returns empty remap");

    std::cout << "\n";
}

// --- C2: IndexPersistence destructor must drain in-flight compaction ---
static void runC2_DestructorDrainTest() {
    std::cout << "========================================\n";
    std::cout << "  C2: Destructor drains in-flight work\n";
    std::cout << "========================================\n\n";

    std::string tmpBase = "/tmp/maceverything_c2_" + std::to_string(getpid()) + ".bin";
    std::string tmpWal = "/tmp/maceverything_c2_" + std::to_string(getpid()) + ".wal";

    // Clean up any leftover files
    fs::remove(tmpBase);
    fs::remove(tmpWal);

    {
        auto engine = std::make_shared<SearchEngine>();
        std::vector<FileRecord> records;
        for (int i = 0; i < 1000; i++) {
            records.push_back({"c2_" + std::to_string(i) + ".txt", "/test", 1, (uint64_t)i, (time_t)i});
        }
        engine->loadRecords(std::move(records));

        IndexPersistence persistence(engine, tmpBase, tmpWal);
        persistence.attachWAL();

        // Start auto-compaction with short interval
        persistence.startAutoCompaction(0.1, nullptr);

        // Wait a bit to let at least one compaction fire
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Destructor should drain without crash
    }

    check(true, "C2: IndexPersistence destroyed without crash");

    // Also test ContentIndexPersistence destructor
    {
        auto index = std::make_shared<ContentIndex>();
        std::string cBase = "/tmp/maceverything_c2c_" + std::to_string(getpid()) + ".bin";
        std::string cWal = "/tmp/maceverything_c2c_" + std::to_string(getpid()) + ".wal";

        ContentIndexPersistence persistence(index, cBase, cWal);
        persistence.attachWAL();
        persistence.startAutoCompaction(0.1);

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Destructor should drain without crash
    }

    check(true, "C2: ContentIndexPersistence destroyed without crash");

    // Cleanup
    fs::remove(tmpBase);
    fs::remove(tmpWal);
    fs::remove("/tmp/maceverything_c2c_" + std::to_string(getpid()) + ".bin");
    fs::remove("/tmp/maceverything_c2c_" + std::to_string(getpid()) + ".wal");

    std::cout << "\n";
}

// --- C3: ContentIndexPersistence WAL append must use walMutex_ ---
static void runC3_WALThreadSafetyTest() {
    std::cout << "========================================\n";
    std::cout << "  C3: WAL append is thread-safe\n";
    std::cout << "========================================\n\n";

    auto index = std::make_shared<ContentIndex>();
    std::string cBase = "/tmp/maceverything_c3_" + std::to_string(getpid()) + ".bin";
    std::string cWal = "/tmp/maceverything_c3_" + std::to_string(getpid()) + ".wal";

    ContentIndexPersistence persistence(index, cBase, cWal);
    persistence.attachWAL();

    std::atomic<bool> running{true};
    std::atomic<bool> errorDetected{false};
    std::atomic<int> opsCount{0};

    // Multiple threads writing to WAL concurrently
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t] {
            int i = 0;
            while (running.load(std::memory_order_relaxed)) {
                try {
                    uint32_t fileIndex = t * 10000 + i;
                    std::vector<Trigram> trigrams = {
                        ContentIndex::makeTrigram('a', 'b', 'c'),
                        ContentIndex::makeTrigram('d', 'e', 'f')
                    };
                    persistence.walAppendAdd(fileIndex, 12345, trigrams);
                    persistence.walAppendRemove(fileIndex);
                    opsCount.fetch_add(2, std::memory_order_relaxed);
                    i++;
                } catch (...) {
                    errorDetected.store(true);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    running.store(false);

    for (auto& th : threads) th.join();

    check(!errorDetected.load(), "C3: no exceptions during concurrent WAL access");
    check(opsCount.load() > 0, "C3: WAL operations completed successfully");
    std::cout << "    Total WAL ops: " << opsCount.load() << "\n";

    // Verify WAL is readable
    auto entries = ContentIndexWAL::readAll(cWal);
    check(!entries.empty(), "C3: WAL entries are readable after concurrent writes");
    std::cout << "    Readable WAL entries: " << entries.size() << "\n";

    // Cleanup
    persistence.stopAutoCompactionAndWait();
    fs::remove(cBase);
    fs::remove(cWal);

    std::cout << "\n";
}

// --- C4: saveToFile must check writeRecord return value ---
static void runC4_SaveFileErrorCheckTest() {
    std::cout << "========================================\n";
    std::cout << "  C4: saveToFile checks write errors\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;
    std::vector<FileRecord> records;
    for (int i = 0; i < 100; i++) {
        records.push_back({"c4_" + std::to_string(i) + ".txt", "/test", 1, (uint64_t)i, (time_t)i});
    }
    engine.loadRecords(std::move(records));

    // Test 1: Normal save/load roundtrip to verify data integrity
    std::string tmpFile = "/tmp/maceverything_c4_" + std::to_string(getpid()) + ".bin";
    IndexMetadata meta;
    meta.lastEventId = 42;

    bool saved = engine.saveToFile(tmpFile, meta);
    check(saved, "C4: saveToFile succeeded");

    SearchEngine engine2;
    IndexMetadata loadedMeta;
    bool loaded = engine2.loadFromFile(tmpFile, &loadedMeta);
    check(loaded, "C4: loadFromFile succeeded");
    check(engine2.liveRecordCount() == 100, "C4: all 100 records preserved in roundtrip");

    // Test 2: save to invalid path should return false
    bool savedBad = engine.saveToFile("/nonexistent/dir/test.bin", meta);
    check(!savedBad, "C4: saveToFile to invalid path returns false");

    // Cleanup
    fs::remove(tmpFile);

    std::cout << "\n";
}

// --- C5: DirectoryScanner must reset state between scans ---
static void runC5_ScannerResetTest() {
    std::cout << "========================================\n";
    std::cout << "  C5: DirectoryScanner resets between scans\n";
    std::cout << "========================================\n\n";

    // Create temp directories with files
    std::string tmpDir1 = "/tmp/maceverything_c5a_" + std::to_string(getpid());
    std::string tmpDir2 = "/tmp/maceverything_c5b_" + std::to_string(getpid());

    fs::create_directories(tmpDir1);
    fs::create_directories(tmpDir2);

    // Create files in each directory
    for (int i = 0; i < 5; i++) {
        std::ofstream(tmpDir1 + "/file_" + std::to_string(i) + ".txt") << "content " << i;
    }
    for (int i = 0; i < 3; i++) {
        std::ofstream(tmpDir2 + "/doc_" + std::to_string(i) + ".md") << "doc " << i;
    }

    DirectoryScanner scanner;

    // First scan
    scanner.scan(tmpDir1);
    auto results1 = scanner.takeResults();
    uint64_t files1 = scanner.getStats().fileCount.load();
    check(files1 >= 5, "C5: first scan found >= 5 files");
    check(!results1.empty(), "C5: first scan has results");

    // Second scan on same scanner instance — this was broken before fix
    scanner.scan(tmpDir2);
    auto results2 = scanner.takeResults();
    uint64_t totalFiles = scanner.getStats().fileCount.load();
    // Stats accumulate across scans, but results2 should only have tmpDir2 files
    // The key check: second scan actually produces results (doesn't exit immediately)
    check(!results2.empty(), "C5: second scan produces results (not empty)");

    // Verify second scan found the tmpDir2 files
    bool foundDoc = false;
    for (const auto& r : results2) {
        if (r.name.find("doc_") != std::string::npos) {
            foundDoc = true;
            break;
        }
    }
    check(foundDoc, "C5: second scan found tmpDir2 files");

    // Cleanup
    fs::remove_all(tmpDir1);
    fs::remove_all(tmpDir2);

    std::cout << "\n";
}

// --- Run all Phase 1 tests ---
static void runPhase1Tests() {
    std::cout << "========================================\n";
    std::cout << "  Phase 1: High-Severity Bug Tests\n";
    std::cout << "========================================\n\n";

    runC1_CompactionRemapTest();
    runC2_DestructorDrainTest();
    runC3_WALThreadSafetyTest();
    runC4_SaveFileErrorCheckTest();
    runC5_ScannerResetTest();
}
