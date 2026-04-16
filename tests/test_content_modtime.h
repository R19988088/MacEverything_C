#pragma once
// Tests for ContentIndex modTime-based incremental indexing (P3 optimization)

#include <fstream>
#include <filesystem>
#include <sys/stat.h>

static void runContentModTimeTests() {
    std::cout << "\n═══ Part 38: Content Index modTime Incremental Indexing ═══\n\n";

    namespace fs = std::filesystem;
    std::string tmpDir = "/tmp/test_content_modtime_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    // Create a test file with known content
    std::string testFile = tmpDir + "/hello.txt";
    {
        std::ofstream ofs(testFile);
        ofs << "hello world test content for trigram indexing";
    }

    // Get the file's actual modTime
    struct stat st;
    stat(testFile.c_str(), &st);
    time_t fileModTime = st.st_mtime;

    // --- Test 1: Same modTime skips reindex ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // First index: should succeed (file is new)
        bool first = idx.indexFile(0, testFile, fileModTime);
        check(first, "First indexFile returns true (new file indexed)");
        check(idx.indexedFileCount() == 1, "File count is 1 after first index");

        // Second index with same modTime: should skip (return false)
        bool second = idx.indexFile(0, testFile, fileModTime);
        check(!second, "Same modTime → indexFile returns false (skipped)");
        check(idx.indexedFileCount() == 1, "File count still 1 after skip");
    }

    // --- Test 2: Changed modTime triggers reindex ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // Index with original modTime
        bool first = idx.indexFile(0, testFile, fileModTime);
        check(first, "First indexFile returns true");

        // Index with different modTime (simulate file modification)
        time_t newModTime = fileModTime + 60; // 1 minute later

        // Modify the file content so hash differs
        {
            std::ofstream ofs(testFile);
            ofs << "modified content with different trigrams for testing";
        }

        bool second = idx.indexFile(0, testFile, newModTime);
        check(second, "Different modTime → indexFile returns true (reindexed)");
        check(idx.indexedFileCount() == 1, "File count still 1 (updated in place)");
    }

    // --- Test 3: modTime=0 does not skip (backward compat with v1 data) ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // Write known content back
        {
            std::ofstream ofs(testFile);
            ofs << "hello world test content for trigram indexing";
        }

        // First index with modTime=0 (simulates v1 persisted data)
        bool first = idx.indexFile(0, testFile, 0);
        check(first, "modTime=0: first indexFile returns true");

        // Second index with modTime=0: should NOT skip (modTime=0 bypasses early exit)
        // Content hash is the same, so it returns false due to hash match, not modTime
        bool second = idx.indexFile(0, testFile, 0);
        check(!second, "modTime=0: second indexFile returns false (hash unchanged, but modTime check bypassed)");

        // Modify file and index again with modTime=0: should return true (hash changed)
        {
            std::ofstream ofs(testFile);
            ofs << "completely different content to change the hash value";
        }
        bool third = idx.indexFile(0, testFile, 0);
        check(third, "modTime=0: returns true when content actually changed");
    }

    // --- Test 4: insertFileInfo preserves lastModTime ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // Directly insert file info with a specific modTime (simulates WAL replay)
        std::vector<Trigram> trigrams = {ContentIndex::makeTrigram('a', 'b', 'c')};
        time_t insertedModTime = 1700000000;
        idx.insertFileInfo(42, 12345, std::move(trigrams), insertedModTime);

        ContentFileInfo info;
        bool got = idx.getFileInfo(42, info);
        check(got, "getFileInfo returns true after insertFileInfo");
        check(info.lastModTime == insertedModTime, "insertFileInfo preserves lastModTime");
    }

    // --- Test 5: Hash-match updates lastModTime when stored value is 0 ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // Write known content
        {
            std::ofstream ofs(testFile);
            ofs << "stable content for modtime upgrade test";
        }

        // First index with modTime=0 (simulates v1 persisted data)
        bool first = idx.indexFile(0, testFile, 0);
        check(first, "T5: first index with modTime=0 returns true");

        ContentFileInfo info;
        idx.getFileInfo(0, info);
        check(info.lastModTime == 0, "T5: stored lastModTime is 0 after modTime=0 index");

        // Second index with real modTime: hash matches but lastModTime differs
        // Should return true (to signal WAL update) and update stored lastModTime
        bool second = idx.indexFile(0, testFile, fileModTime);
        check(second, "T5: hash-match with new modTime returns true (triggers WAL persist)");

        idx.getFileInfo(0, info);
        check(info.lastModTime == fileModTime, "T5: stored lastModTime updated to real modTime");

        // Third index with same modTime: now should skip entirely (no I/O)
        bool third = idx.indexFile(0, testFile, fileModTime);
        check(!third, "T5: third call with same modTime skips (modTime early exit)");
    }

    // --- Test 6: Unreadable file (deleted) updates lastModTime for indexed entry ---
    {
        ContentIndex idx;
        idx.setExtensions({"txt"});

        // Write and index a file normally
        std::string delFile = tmpDir + "/willdelete.txt";
        {
            std::ofstream ofs(delFile);
            ofs << "content that will be deleted soon";
        }

        struct stat dst;
        stat(delFile.c_str(), &dst);
        time_t delModTime = dst.st_mtime;

        bool first = idx.indexFile(0, delFile, delModTime);
        check(first, "T6: first indexFile returns true");
        check(idx.indexedFileCount() == 1, "T6: file count is 1");

        // Delete the file, then re-index with a new modTime
        fs::remove(delFile);
        time_t newMod = delModTime + 120;

        bool second = idx.indexFile(0, delFile, newMod);
        check(second, "T6: deleted file → indexFile returns true (lastModTime updated)");

        ContentFileInfo info;
        idx.getFileInfo(0, info);
        check(info.lastModTime == newMod, "T6: stored lastModTime updated despite file unreadable");

        // Third call with same modTime should skip via early exit
        bool third = idx.indexFile(0, delFile, newMod);
        check(!third, "T6: third call with same modTime skips (early exit)");
    }

    // Cleanup
    fs::remove_all(tmpDir);

    std::cout << "\n  Part 38 done.\n";
}
