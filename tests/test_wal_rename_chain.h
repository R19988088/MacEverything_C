#pragma once
// Test: WAL rename self-propagating failure chain fix
//
// Root cause: After a rename failure, oldWal->closeAndDelete() in the next
// compact cycle deletes the .wal.new file that the current WAL is using,
// causing all subsequent renames to fail with ENOENT.
//
// The fix reorders compact steps: rename happens before closeAndDelete,
// so the new WAL's directory entry is always at the standard path when
// the old WAL is deleted.

static void runWalRenameChainTest() {
    std::cout << "=== WAL Rename Chain Fix ===" << std::endl;

    std::string tmpDir = "/tmp/maceverything_wal_rename_chain_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    // --- Test ContentIndexPersistence compact rename ---
    {
        auto contentIndex = std::make_shared<ContentIndex>();
        std::string basePath = tmpDir + "/content_index.bin";
        std::string walPath = tmpDir + "/content_index.wal";

        ContentIndexPersistence persistence(contentIndex, basePath, walPath);
        persistence.load();
        persistence.attachWAL();

        // First compact — should succeed cleanly
        persistence.compact();
        check(fs::exists(basePath), "RC1: base file created after first compact");
        // WAL should be at standard path, not .new
        check(fs::exists(walPath), "RC1: WAL at standard path after compact");
        check(!fs::exists(walPath + ".new"), "RC1: no leftover .wal.new after compact");

        // Second compact — regression: previously this would fail because
        // closeAndDelete on old WAL would unlink the new WAL's file
        persistence.compact();
        check(fs::exists(basePath), "RC2: base file still exists after second compact");
        check(fs::exists(walPath), "RC2: WAL at standard path after second compact");
        check(!fs::exists(walPath + ".new"), "RC2: no leftover .wal.new after second compact");

        // Third compact — ensure no accumulated failures
        persistence.compact();
        check(fs::exists(walPath), "RC3: WAL at standard path after third compact");
        check(!fs::exists(walPath + ".new"), "RC3: no leftover .wal.new after third compact");
    }

    // --- Test IndexPersistence compact rename ---
    {
        auto engine = std::make_shared<SearchEngine>();
        std::string basePath = tmpDir + "/index.bin";
        std::string walPath = tmpDir + "/index.wal";

        // Add some records so saveToFile has data to write
        FileRecord rec;
        rec.name = "test.txt";
        rec.path = tmpDir;
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);
        engine->addRecord(std::move(rec));

        IndexPersistence persistence(engine, basePath, walPath);
        persistence.attachWAL();

        IndexMetadata meta;
        meta.lastEventId = 1;

        // Multiple compacts in sequence — all should succeed
        persistence.compact(meta);
        check(fs::exists(walPath), "RI1: WAL at standard path after first compact");
        check(!fs::exists(walPath + ".new"), "RI1: no leftover .wal.new");

        meta.lastEventId = 2;
        persistence.compact(meta);
        check(fs::exists(walPath), "RI2: WAL at standard path after second compact");
        check(!fs::exists(walPath + ".new"), "RI2: no leftover .wal.new");

        meta.lastEventId = 3;
        persistence.compact(meta);
        check(fs::exists(walPath), "RI3: WAL at standard path after third compact");
        check(!fs::exists(walPath + ".new"), "RI3: no leftover .wal.new");
    }

    fs::remove_all(tmpDir);
    std::cout << std::endl;
}
