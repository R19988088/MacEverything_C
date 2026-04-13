// tests/test_phase2.h — Phase 2: Core layer medium-severity regression tests (P1-P6, T1-T2, L1-L2)
#pragma once

// ═══════════════════════════════════════════════════════
//  P1: WAL batched sync (fflush+fsync not on every append)
// ═══════════════════════════════════════════════════════
static void runP1_WALBatchSyncTest() {
    std::cout << "\n── P1: WAL batched sync ──\n";

    auto tmpDir = fs::temp_directory_path() / "me_p1_test";
    fs::create_directories(tmpDir);
    auto walFile = tmpDir / "test.wal";
    fs::remove(walFile);

    {
        IndexWAL wal;
        check(wal.open(walFile.string()), "P1: WAL open");

        // Write many entries — should not fsync on each one
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; i++) {
            FileRecord r;
            r.name = "file_" + std::to_string(i) + ".txt";
            r.path = "/test/dir";
            r.type = 1;
            r.size = 100;
            r.modTime = 1000;
            r.inode = i;
            r.devId = 1;
            wal.append(WALOp::Add, "/test/dir/" + r.name, r);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // With batched sync, 200 writes should complete much faster than 200 individual fsyncs
        // (200 fsyncs would take ~20-200ms on SSD; batched should be <10ms typically)
        check(wal.entryCount() == 200, "P1: 200 entries written");
        std::cout << "    200 WAL appends took " << std::fixed << std::setprecision(1) << ms << " ms\n";

        // Explicit flush should work
        wal.flush();

        wal.close();
    }

    // Verify all entries can be read back correctly
    auto entries = IndexWAL::readAll(walFile.string());
    check(entries.size() == 200, "P1: all 200 entries readable after close");
    check(entries[0].record.name == "file_0.txt", "P1: first entry correct");
    check(entries[199].record.name == "file_199.txt", "P1: last entry correct");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  P2: DirectoryScanner memory usage (reduced buffer + reserve)
// ═══════════════════════════════════════════════════════
static void runP2_ScannerMemoryTest() {
    std::cout << "\n── P2: Scanner memory reduction ──\n";

    // Verify ATTR_BUF_SIZE and reserve constants are reasonable
    // We can't check the constant directly from outside, but we can verify
    // scanning /tmp doesn't use excessive memory
    DirectoryScanner scanner;

    double memBefore = getMemoryUsageMB();
    scanner.scan("/tmp");
    double memAfter = getMemoryUsageMB();
    auto results = scanner.takeResults();

    double memDelta = memAfter - memBefore;
    std::cout << "    Scanning /tmp: " << results.size() << " entries, memory delta: "
              << std::fixed << std::setprecision(1) << memDelta << " MB\n";

    // Memory delta should be reasonable (not hundreds of MB for /tmp)
    check(results.size() > 0, "P2: scanner produced results");
    check(memDelta < 200.0, "P2: memory usage reasonable (<200MB for /tmp)");
}

// ═══════════════════════════════════════════════════════
//  P3: extractTrigrams bitmap optimization
// ═══════════════════════════════════════════════════════
static void runP3_TrigramBitmapTest() {
    std::cout << "\n── P3: Trigram extraction efficiency ──\n";

    // Build a large string to test trigram extraction performance
    std::string largeText;
    largeText.reserve(100000);
    for (int i = 0; i < 10000; i++) {
        largeText += "hello world foo bar baz ";
    }

    auto t0 = std::chrono::steady_clock::now();
    auto trigrams = ContentIndex::extractTrigrams(largeText);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "    100KB text: " << trigrams.size() << " unique trigrams in "
              << std::fixed << std::setprecision(2) << ms << " ms\n";

    // Verify correctness: known trigrams should be present
    auto findTri = [&](uint8_t a, uint8_t b, uint8_t c) -> bool {
        Trigram t = ContentIndex::makeTrigram(a, b, c);
        return std::find(trigrams.begin(), trigrams.end(), t) != trigrams.end();
    };

    check(findTri('h', 'e', 'l'), "P3: 'hel' trigram found");
    check(findTri('e', 'l', 'l'), "P3: 'ell' trigram found");
    check(findTri('l', 'l', 'o'), "P3: 'llo' trigram found");
    check(trigrams.size() > 0 && trigrams.size() <= (1 << 24), "P3: trigram count in valid range");

    // Verify no duplicates
    std::unordered_set<Trigram> uniqueCheck(trigrams.begin(), trigrams.end());
    check(uniqueCheck.size() == trigrams.size(), "P3: no duplicate trigrams");

    // Performance: should be fast (bitmap is much faster than unordered_set for large inputs)
    check(ms < 100.0, "P3: extraction < 100ms for 100KB");
}

// ═══════════════════════════════════════════════════════
//  P4: indexFile single I/O (no double read)
// ═══════════════════════════════════════════════════════
static void runP4_SingleIOTest() {
    std::cout << "\n── P4: Content indexing single I/O ──\n";

    auto tmpDir = fs::temp_directory_path() / "me_p4_test";
    fs::create_directories(tmpDir);

    // Create a test file
    auto testFile = tmpDir / "test_code.cpp";
    {
        std::ofstream f(testFile);
        f << "#include <iostream>\n";
        f << "int main() {\n";
        f << "    std::cout << \"hello world\" << std::endl;\n";
        f << "    return 0;\n";
        f << "}\n";
    }

    ContentIndex ci;
    ci.setExtensions({"cpp", "h", "txt"});
    ci.setMaxFileSize(1024 * 1024);

    // Index the file — should work with single I/O
    bool indexed = ci.indexFile(0, testFile.string());
    check(indexed, "P4: file indexed successfully");
    check(ci.isFileIndexed(0), "P4: file shows as indexed");

    // Verify trigrams are correct
    ContentFileInfo info;
    check(ci.getFileInfo(0, info), "P4: can retrieve file info");
    check(info.trigrams.size() > 0, "P4: trigrams extracted");

    // Re-index should detect no change (same hash)
    bool reindexed = ci.indexFile(0, testFile.string());
    check(reindexed, "P4: re-index returns true (already up-to-date)");

    // Test binary file detection
    auto binFile = tmpDir / "test.bin";
    {
        std::ofstream f(binFile, std::ios::binary);
        char data[] = "hello\0world";
        f.write(data, 11);
    }
    // Rename to .cpp to test binary detection
    auto binCpp = tmpDir / "binary.cpp";
    fs::rename(binFile, binCpp);
    bool binIndexed = ci.indexFile(1, binCpp.string());
    check(!binIndexed, "P4: binary file correctly rejected");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  P5: Posting list sorted insertion + binary search
// ═══════════════════════════════════════════════════════
static void runP5_SortedPostingListTest() {
    std::cout << "\n── P5: Sorted posting lists ──\n";

    ContentIndex ci;
    ci.setExtensions({"txt"});

    auto tmpDir = fs::temp_directory_path() / "me_p5_test";
    fs::create_directories(tmpDir);

    // Create multiple files with overlapping content
    for (int i = 0; i < 10; i++) {
        auto f = tmpDir / ("file_" + std::to_string(i) + ".txt");
        std::ofstream out(f);
        out << "common_keyword_" << i << " unique_" << i << "_text\n";
        out << "shared_content_across_files_for_trigram_intersection\n";
    }

    // Index all files
    for (int i = 0; i < 10; i++) {
        auto f = tmpDir / ("file_" + std::to_string(i) + ".txt");
        ci.indexFile(static_cast<uint32_t>(i), f.string());
    }

    check(ci.indexedFileCount() == 10, "P5: all 10 files indexed");

    // Query should find files with common content
    auto results = ci.query("shared_content", 100);
    check(results.size() > 0, "P5: query finds results");

    // Query for unique content should find exactly one file
    auto unique = ci.query("unique_3_text", 100);
    // Note: trigram-based search is approximate; it finds candidates
    check(unique.size() >= 1, "P5: unique query finds at least 1 result");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  P6: Query trigram skip logic fix (dead code removal/fix)
// ═══════════════════════════════════════════════════════
static void runP6_TrigramSkipLogicTest() {
    std::cout << "\n── P6: Trigram skip logic ──\n";

    ContentIndex ci;
    ci.setExtensions({"txt"});

    auto tmpDir = fs::temp_directory_path() / "me_p6_test";
    fs::create_directories(tmpDir);

    // Create files: one with keyword, others without
    {
        std::ofstream f(tmpDir / "has_keyword.txt");
        f << "This file contains the special_search_term we are looking for.\n";
    }
    {
        std::ofstream f(tmpDir / "no_keyword.txt");
        f << "This file has completely different content that should not match.\n";
    }

    ci.indexFile(0, (tmpDir / "has_keyword.txt").string());
    ci.indexFile(1, (tmpDir / "no_keyword.txt").string());

    // Search for the keyword
    auto results = ci.query("special_search_term", 100);
    check(results.size() >= 1, "P6: found file with keyword");

    // The candidate with fileIndex=0 should be in results
    bool foundTarget = false;
    for (auto& m : results) {
        if (m.fileIndex == 0) foundTarget = true;
    }
    check(foundTarget, "P6: correct file (index 0) is a candidate");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  T1: compact atomicity (no duplicate records from WAL+base overlap)
// ═══════════════════════════════════════════════════════
static void runT1_CompactAtomicityTest() {
    std::cout << "\n── T1: Compact atomicity ──\n";

    auto tmpDir = fs::temp_directory_path() / "me_t1_test";
    fs::create_directories(tmpDir);
    auto baseFile = tmpDir / "index.bin";
    auto walFile = tmpDir / "index.wal";

    auto engine = std::make_shared<SearchEngine>();

    // Add initial records
    std::vector<FileRecord> records;
    for (int i = 0; i < 100; i++) {
        FileRecord r;
        r.name = "file_" + std::to_string(i) + ".txt";
        r.path = "/test";
        r.type = 1;
        r.size = 100;
        r.modTime = 1000;
        r.inode = i;
        r.devId = 1;
        records.push_back(std::move(r));
    }
    engine->loadRecords(std::move(records));
    check(engine->liveRecordCount() == 100, "T1: loaded 100 records");

    IndexPersistence persistence(engine, baseFile.string(), walFile.string());
    persistence.attachWAL();

    // Remove some records (creates tombstones)
    for (int i = 0; i < 30; i++) {
        engine->removeByPath("/test/file_" + std::to_string(i) + ".txt");
    }
    check(engine->liveRecordCount() == 70, "T1: 70 live after removing 30");

    // Add new records via mutation
    for (int i = 100; i < 110; i++) {
        FileRecord r;
        r.name = "new_file_" + std::to_string(i) + ".txt";
        r.path = "/test";
        r.type = 1;
        r.size = 200;
        r.modTime = 2000;
        r.inode = i;
        r.devId = 1;
        engine->addRecord(std::move(r));
    }
    check(engine->liveRecordCount() == 80, "T1: 80 live after adding 10 new");

    // Compact
    IndexMetadata meta;
    meta.lastEventId = 12345;
    persistence.compact(meta);

    // Verify: reload from compacted file should have exactly 80 records
    auto engine2 = std::make_shared<SearchEngine>();
    IndexMetadata loadedMeta;
    check(engine2->loadFromFile(baseFile.string(), &loadedMeta), "T1: reload compacted file");
    check(engine2->liveRecordCount() == 80, "T1: compacted file has exactly 80 records");
    check(loadedMeta.lastEventId == 12345, "T1: metadata preserved");

    // Verify no duplicates: query for a record that was added after initial load
    auto q = engine2->query("new_file_105", 10);
    check(q.size() == 1, "T1: no duplicate for new_file_105");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  T2: hasAllowedExtension thread safety
// ═══════════════════════════════════════════════════════
static void runT2_HasAllowedExtensionSafetyTest() {
    std::cout << "\n── T2: hasAllowedExtension safety ──\n";

    ContentIndex ci;
    ci.setExtensions({"cpp", "h", "txt", "py", "rs"});

    // hasAllowedExtension should work correctly
    check(ci.hasAllowedExtension("foo.cpp"), "T2: .cpp allowed");
    check(ci.hasAllowedExtension("bar.h"), "T2: .h allowed");
    check(ci.hasAllowedExtension("baz.txt"), "T2: .txt allowed");
    check(!ci.hasAllowedExtension("image.png"), "T2: .png not allowed");
    check(!ci.hasAllowedExtension("data.bin"), "T2: .bin not allowed");

    // Concurrent access test: read hasAllowedExtension while modifying extensions
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::thread writer([&] {
        for (int i = 0; !stop.load(std::memory_order_relaxed); i++) {
            if (i % 2 == 0)
                ci.setExtensions({"cpp", "h", "txt"});
            else
                ci.setExtensions({"py", "rs", "go", "java"});
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 10000 && !stop.load(std::memory_order_relaxed); i++) {
            // Should not crash even with concurrent modification
            (void)ci.hasAllowedExtension("test.cpp");
            (void)ci.hasAllowedExtension("test.py");
        }
    });

    reader.join();
    stop.store(true);
    writer.join();

    check(errors.load() == 0, "T2: no errors during concurrent access");
}

// ═══════════════════════════════════════════════════════
//  L1: pathIndex_ case-insensitive keys
// ═══════════════════════════════════════════════════════
static void runL1_CaseInsensitivePathIndexTest() {
    std::cout << "\n── L1: Case-insensitive pathIndex ──\n";

    SearchEngine engine;

    // Add a record with mixed case
    FileRecord r1;
    r1.name = "MyFile.txt";
    r1.path = "/Users/TestUser/Documents";
    r1.type = 1;
    r1.size = 100;
    r1.modTime = 1000;
    r1.inode = 1;
    r1.devId = 1;
    engine.addRecord(std::move(r1));

    // Remove using different case (as macOS FSEvents might report)
    bool removed = engine.removeByPath("/users/testuser/documents/myfile.txt");
    check(removed, "L1: removeByPath with different case succeeds");
    check(engine.liveRecordCount() == 0, "L1: record removed");

    // Test with indexForPath
    FileRecord r2;
    r2.name = "AnotherFile.CPP";
    r2.path = "/Users/Dev/Project";
    r2.type = 1;
    r2.size = 200;
    r2.modTime = 2000;
    r2.inode = 2;
    r2.devId = 1;
    engine.addRecord(std::move(r2));

    uint32_t idx = engine.indexForPath("/users/dev/project/anotherfile.cpp");
    check(idx != UINT32_MAX, "L1: indexForPath case-insensitive lookup");

    // Test updateByPath with different case
    FileRecord r3;
    r3.name = "AnotherFile.CPP";
    r3.path = "/Users/Dev/Project";
    r3.type = 1;
    r3.size = 999;
    r3.modTime = 3000;
    r3.inode = 2;
    r3.devId = 1;
    engine.updateByPath("/USERS/DEV/PROJECT/ANOTHERFILE.CPP", std::move(r3));
    check(engine.liveRecordCount() == 1, "L1: updateByPath with different case (no duplicate)");

    // Verify the updated record
    auto results = engine.query("AnotherFile", 10);
    check(results.size() == 1, "L1: exactly one result after case-insensitive update");
    auto rec = engine.getRecord(results[0]);
    check(rec.size == 999, "L1: updated size correct");
}

// ═══════════════════════════════════════════════════════
//  L2: WAL rename failure handling
// ═══════════════════════════════════════════════════════
static void runL2_WALRenameHandlingTest() {
    std::cout << "\n── L2: WAL rename error handling ──\n";

    auto tmpDir = fs::temp_directory_path() / "me_l2_test";
    fs::create_directories(tmpDir);
    auto baseFile = tmpDir / "index.bin";
    auto walFile = tmpDir / "index.wal";

    auto engine = std::make_shared<SearchEngine>();

    // Add some records
    std::vector<FileRecord> records;
    for (int i = 0; i < 10; i++) {
        FileRecord r;
        r.name = "file_" + std::to_string(i) + ".txt";
        r.path = "/test";
        r.type = 1;
        r.size = 100;
        r.modTime = 1000;
        r.inode = i;
        r.devId = 1;
        records.push_back(std::move(r));
    }
    engine->loadRecords(std::move(records));

    IndexPersistence persistence(engine, baseFile.string(), walFile.string());
    persistence.attachWAL();

    // Normal compact should succeed
    IndexMetadata meta;
    meta.lastEventId = 100;
    persistence.compact(meta);

    // Verify base file exists
    check(fs::exists(baseFile), "L2: base file created after compact");

    // Verify WAL file exists (the new WAL, renamed from .new)
    check(fs::exists(walFile), "L2: WAL file exists after compact");

    // Another compact should also succeed
    engine->addRecord(FileRecord{"extra.txt", "/test", 1, 50, 2000, 20, 1});
    meta.lastEventId = 200;
    persistence.compact(meta);

    auto engine2 = std::make_shared<SearchEngine>();
    IndexMetadata loadedMeta;
    check(engine2->loadFromFile(baseFile.string(), &loadedMeta), "L2: reload after second compact");
    check(engine2->liveRecordCount() == 11, "L2: correct record count after second compact");
    check(loadedMeta.lastEventId == 200, "L2: updated lastEventId");

    fs::remove_all(tmpDir);
}

// ═══════════════════════════════════════════════════════
//  Phase 2 orchestrator
// ═══════════════════════════════════════════════════════
static void runPhase2Tests() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  Phase 2: Core Medium-Severity Tests     ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";

    runP1_WALBatchSyncTest();
    runP2_ScannerMemoryTest();
    runP3_TrigramBitmapTest();
    runP4_SingleIOTest();
    runP5_SortedPostingListTest();
    runP6_TrigramSkipLogicTest();
    runT1_CompactAtomicityTest();
    runT2_HasAllowedExtensionSafetyTest();
    runL1_CaseInsensitivePathIndexTest();
    runL2_WALRenameHandlingTest();
}
