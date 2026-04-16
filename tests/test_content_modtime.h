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

    // Cleanup
    fs::remove_all(tmpDir);

    std::cout << "\n  Part 38 done.\n";
}
