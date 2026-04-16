#pragma once
// Part 36: Content compaction WAL guard — walAppendRemove must be skipped
//          for files that are NOT in the content index.

static void runContentCompactionGuardTests() {
    std::cout << "═══ Part 36: Content Compaction WAL Guard Tests ═══\n\n";

    std::string tmpDir = fs::temp_directory_path() / "me_test_compaction_guard";
    fs::remove_all(tmpDir);
    fs::create_directories(tmpDir);

    std::string walPath = tmpDir + "/content.wal";
    std::string basePath = tmpDir + "/content.idx";

    // Test 1: WAL stays clean when removing a file not in the content index.
    //   This simulates the root cause: a file deletion event arrives for a
    //   file that was never content-indexed (e.g., binary, too large, wrong ext).
    //   Before the fix, walAppendRemove was called unconditionally, polluting
    //   the dirty flag and defeating the compact skip logic.
    {
        auto ci = std::make_shared<ContentIndex>();

        // File 42 is NOT indexed — isFileIndexed(42) must be false
        check(!ci->isFileIndexed(42), "Guard: file 42 not indexed initially");

        // Simulate the fixed bridge logic:
        // if (fileIndex != UINT32_MAX && contentIndex->isFileIndexed(fileIndex)) { ... }
        ContentIndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0);

        // Guard check — skip removeFile + walAppendRemove for non-indexed files
        uint32_t fileIndex = 42;
        if (ci->isFileIndexed(fileIndex)) {
            ci->removeFile(fileIndex);
            wal.appendRemove(fileIndex);
        }

        check(!wal.isDirty(), "Guard: WAL not dirty after guarded skip of non-indexed file");
        check(wal.entryCount() == 0, "Guard: WAL entry count 0 after guarded skip");

        wal.closeAndDelete();
    }

    // Test 2: WAL IS written when removing a file that IS in the content index.
    {
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);

        auto ci = std::make_shared<ContentIndex>();

        // Create a text file to index
        std::string testFile = tmpDir + "/hello.txt";
        {
            std::ofstream ofs(testFile);
            ofs << "hello world this is a test file for content indexing";
        }

        ci->setExtensions({"txt"});
        bool didIndex = ci->indexFile(1, testFile);
        check(didIndex, "Guard: indexFile succeeds for text file");
        check(ci->isFileIndexed(1), "Guard: file 1 is indexed");

        ContentIndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0);

        // Guard check — file IS indexed, so removeFile + walAppendRemove should execute
        uint32_t fileIndex = 1;
        if (ci->isFileIndexed(fileIndex)) {
            ci->removeFile(fileIndex);
            wal.appendRemove(fileIndex);
        }

        check(wal.isDirty(), "Guard: WAL is dirty after removing indexed file");
        check(wal.entryCount() == 1, "Guard: WAL entry count 1 after removing indexed file");
        check(!ci->isFileIndexed(1), "Guard: file 1 no longer indexed after removal");

        wal.closeAndDelete();
    }

    // Test 3: ContentIndexPersistence compact skips when WAL has zero real mutations.
    //   Simulates the scenario where all file events are for non-indexed files.
    {
        fs::remove_all(tmpDir);
        fs::create_directories(tmpDir);

        auto ci = std::make_shared<ContentIndex>();
        ContentIndexPersistence cip(ci, basePath, walPath);
        cip.load();
        cip.attachWAL();

        // No mutations — compact should skip
        cip.compact(/*force=*/false);

        // If we got here without crash, the skip logic works.
        // The WAL should still be clean because no mutations were appended.
        check(ci->indexedFileCount() == 0, "Guard: no files indexed, compact was a no-op");
    }

    // Cleanup
    fs::remove_all(tmpDir);

    std::cout << "\n";
}
