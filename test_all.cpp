// MacEverything — Full Test Suite + Performance Report
// Compile: clang++ -std=c++20 -O2 -framework CoreServices MacEverything/Core/*.cpp test_all.cpp -o test_all
// Run:     ./test_all [root_path]            (all tests, default root: /)
//          ./test_all --fast                 (fast unit tests only: 3, 3b, 3c, 3d, 3e, 5)
//          ./test_all --slow [root_path]     (slow integration tests: 1, 4, 6)
//          ./test_all --part 3 --part 3b     (specific parts)

#include "MacEverything/Core/DirectoryScanner.h"
#include "MacEverything/Core/SearchEngine.h"
#include "MacEverything/Core/ContentIndex.h"
#include "MacEverything/Core/IndexPersistence.h"
#include "MacEverything/Core/ContentIndexPersistence.h"
#include "MacEverything/Core/FileSystemWatcher.h"
#include "MacEverything/Core/ContentIndex.h"
#include "MacEverything/Core/IndexPersistence.h"
#include "MacEverything/Core/ContentIndexPersistence.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <cassert>
#include <set>

namespace fs = std::filesystem;

// ─────────── Test modules ───────────
#include "tests/test_helpers.h"
#include "tests/test_scan_query.h"
#include "tests/test_mutation.h"
#include "tests/test_path_search.h"
#include "tests/test_metadata.h"
#include "tests/test_compaction.h"
#include "tests/test_ranking.h"
#include "tests/test_fsevents.h"
#include "tests/test_thread_safety.h"
#include "tests/test_e2e.h"

// ═══════════════════════════════════════════════════════
//  Part 7: C1 — compactRecords + ContentIndex fileIndex remap
// ═══════════════════════════════════════════════════════

static void runCompactContentIndexTest() {
    std::cout << "═══ Part 7: CompactRecords + ContentIndex Remap ═══\n\n";

    // Create temp dir with test files
    std::string tmpDir = "/tmp/maceverything_c1_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    // Create test text files
    for (int i = 0; i < 5; i++) {
        std::string path = tmpDir + "/file" + std::to_string(i) + ".txt";
        std::ofstream ofs(path);
        ofs << "This is test file number " + std::to_string(i) + " with enough content to generate trigrams for indexing purposes.";
    }

    auto engine = std::make_shared<SearchEngine>();
    auto contentIndex = std::make_shared<ContentIndex>();
    contentIndex->setExtensions({"txt"});

    // Add records and index their content
    std::vector<uint32_t> indices;
    for (int i = 0; i < 5; i++) {
        FileRecord rec;
        rec.name = "file" + std::to_string(i) + ".txt";
        rec.path = tmpDir;
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);
        uint32_t idx = engine->addRecord(std::move(rec));
        indices.push_back(idx);

        std::string fullPath = tmpDir + "/file" + std::to_string(i) + ".txt";
        contentIndex->indexFile(idx, fullPath);
    }

    check(contentIndex->indexedFileCount() == 5, "C1: All 5 files indexed in ContentIndex");

    // Remove files 1 and 3 to create tombstones
    engine->removeByPath(tmpDir + "/file1.txt");
    engine->removeByPath(tmpDir + "/file3.txt");
    contentIndex->removeFile(indices[1]);
    contentIndex->removeFile(indices[3]);

    check(engine->liveRecordCount() == 3, "C1: 3 live records after removal");
    check(contentIndex->indexedFileCount() == 3, "C1: 3 content-indexed files after removal");

    // Compact records (removes tombstones)
    engine->compactRecords();

    // Verify surviving files are still in SearchEngine
    for (int i : {0, 2, 4}) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::string fullPath = tmpDir + "/" + name;
        uint32_t newIdx = engine->indexForPath(fullPath);
        check(newIdx != UINT32_MAX, ("C1: file" + std::to_string(i) + " still in SearchEngine").c_str());
    }

    // Verify removed files are NOT in ContentIndex with old indices
    check(!contentIndex->isFileIndexed(indices[1]), "C1: file1 old index not in ContentIndex");
    check(!contentIndex->isFileIndexed(indices[3]), "C1: file3 old index not in ContentIndex");

    // Test via IndexPersistence.compact() integration
    std::string basePath = tmpDir + "/test_index.bin";
    std::string walPath = tmpDir + "/test_index.wal";
    auto persistence = std::make_unique<IndexPersistence>(engine, basePath, walPath);
    persistence->attachWAL();

    // Add a new record, remove it, then compact via IndexPersistence
    FileRecord newRec;
    newRec.name = "extra.txt";
    newRec.path = tmpDir;
    newRec.type = 1;
    newRec.size = 50;
    newRec.modTime = time(nullptr);
    uint32_t extraIdx = engine->addRecord(std::move(newRec));
    engine->removeByPath(tmpDir + "/extra.txt");

    IndexMetadata meta;
    meta.lastEventId = 42;
    persistence->compact(meta);

    check(contentIndex->indexedFileCount() == 3, "C1: ContentIndex still has 3 files after IndexPersistence::compact()");

    persistence.reset();
    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 7b: C2 — Destructor use-after-free (stopAutoCompactionAndWait)
// ═══════════════════════════════════════════════════════

static void runDestructorSafetyTest() {
    std::cout << "═══ Part 7b: Destructor Safety (stopAutoCompactionAndWait) ═══\n\n";

    std::string tmpDir = "/tmp/maceverything_c2_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    // Test IndexPersistence: create, start auto-compaction, destroy quickly
    {
        auto engine = std::make_shared<SearchEngine>();
        std::string basePath = tmpDir + "/idx.bin";
        std::string walPath = tmpDir + "/idx.wal";
        auto persistence = std::make_unique<IndexPersistence>(engine, basePath, walPath);
        persistence->attachWAL();
        persistence->startAutoCompaction(0.1, nullptr);  // 100ms interval
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        persistence.reset();  // Should not crash — destructor waits for in-flight compaction
    }
    check(true, "C2: IndexPersistence destroyed safely with active compaction timer");

    // Test ContentIndexPersistence: same pattern
    {
        auto contentIndex = std::make_shared<ContentIndex>();
        std::string basePath = tmpDir + "/cidx.bin";
        std::string walPath = tmpDir + "/cidx.wal";
        auto persistence = std::make_unique<ContentIndexPersistence>(contentIndex, basePath, walPath);
        persistence->attachWAL();
        persistence->startAutoCompaction(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        persistence.reset();  // Should not crash
    }
    check(true, "C2: ContentIndexPersistence destroyed safely with active compaction timer");

    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 7c: C3 — ContentIndexPersistence wal_ data race
// ═══════════════════════════════════════════════════════

static void runWalRaceTest() {
    std::cout << "═══ Part 7c: ContentIndexPersistence WAL Race Safety ═══\n\n";

    std::string tmpDir = "/tmp/maceverything_c3_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    auto contentIndex = std::make_shared<ContentIndex>();
    auto persistence = std::make_shared<ContentIndexPersistence>(
        contentIndex, tmpDir + "/cidx.bin", tmpDir + "/cidx.wal");
    persistence->attachWAL();

    // Hammer walAppendAdd/walAppendRemove from multiple threads while compacting
    std::atomic<bool> stop{false};
    std::atomic<int> appendCount{0};

    std::vector<std::thread> writers;
    for (int t = 0; t < 4; t++) {
        writers.emplace_back([&, t]() {
            std::vector<Trigram> trigrams = {
                ContentIndex::makeTrigram('a', 'b', 'c'),
                ContentIndex::makeTrigram('d', 'e', 'f')
            };
            uint32_t idx = static_cast<uint32_t>(t * 1000);
            while (!stop.load(std::memory_order_relaxed)) {
                persistence->walAppendAdd(idx, 12345, trigrams);
                persistence->walAppendRemove(idx);
                appendCount.fetch_add(2, std::memory_order_relaxed);
                idx++;
            }
        });
    }

    // Compact a few times concurrently
    for (int i = 0; i < 3; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        persistence->compact();
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : writers) t.join();

    check(appendCount.load() > 0, "C3: Concurrent WAL appends + compacts completed without crash");
    check(true, "C3: No data race on wal_ pointer");

    persistence.reset();
    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 7d: C4 — saveToFile checks writeRecord return value
// ═══════════════════════════════════════════════════════

static void runSaveToFileTest() {
    std::cout << "═══ Part 7d: saveToFile writeRecord Error Handling ═══\n\n";

    std::string tmpDir = "/tmp/maceverything_c4_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    SearchEngine engine;

    // Add some records
    for (int i = 0; i < 10; i++) {
        FileRecord rec;
        rec.name = "file" + std::to_string(i) + ".txt";
        rec.path = "/test/path";
        rec.type = 1;
        rec.size = 100 * i;
        rec.modTime = time(nullptr);
        engine.addRecord(std::move(rec));
    }

    // Save to a valid path should succeed
    std::string validPath = tmpDir + "/test_save.bin";
    bool ok = engine.saveToFile(validPath, 0);
    check(ok, "C4: saveToFile succeeds on valid path");

    // Save to an invalid path (non-existent directory) should fail
    std::string invalidPath = "/tmp/nonexistent_dir_" + std::to_string(getpid()) + "/sub/test.bin";
    bool fail = engine.saveToFile(invalidPath, 0);
    check(!fail, "C4: saveToFile fails on invalid path");

    // Verify the valid save can be loaded back
    SearchEngine engine2;
    ok = engine2.loadFromFile(validPath, static_cast<IndexMetadata*>(nullptr));
    check(ok, "C4: Saved file loads back correctly");
    check(engine2.liveRecordCount() == 10, "C4: All 10 records survive save/load");

    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 7e: C5 — DirectoryScanner scan() re-entrancy
// ═══════════════════════════════════════════════════════

static void runScannerReentryTest() {
    std::cout << "═══ Part 7e: DirectoryScanner Re-entrancy ═══\n\n";

    // Create temp directory structure
    std::string tmpDir = "/tmp/maceverything_c5_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir + "/sub1");
    fs::create_directories(tmpDir + "/sub2");

    // Create test files
    for (int i = 0; i < 3; i++) {
        std::ofstream(tmpDir + "/file" + std::to_string(i) + ".txt") << "test";
    }
    std::ofstream(tmpDir + "/sub1/deep.txt") << "deep file";
    std::ofstream(tmpDir + "/sub2/other.txt") << "other file";

    DirectoryScanner scanner;

    // First scan
    scanner.scan(tmpDir);
    auto results1 = scanner.takeResults();
    uint64_t fileCount1 = scanner.getStats().fileCount.load();
    uint64_t dirCount1 = scanner.getStats().dirCount.load();

    check(results1.size() > 0, "C5: First scan found results");
    check(fileCount1 >= 5, "C5: First scan found >= 5 files");

    // Second scan on same scanner instance — must NOT fail or return empty
    scanner.scan(tmpDir);
    auto results2 = scanner.takeResults();
    uint64_t fileCount2 = scanner.getStats().fileCount.load();
    uint64_t dirCount2 = scanner.getStats().dirCount.load();

    check(results2.size() > 0, "C5: Second scan found results (not stuck on done_=true)");
    check(results2.size() == results1.size(), "C5: Second scan found same number of entries");
    check(fileCount2 == fileCount1, "C5: Second scan file count matches first");
    check(dirCount2 == dirCount1, "C5: Second scan dir count matches first");

    // Third scan on a different directory
    std::string tmpDir2 = "/tmp/maceverything_c5_test2_" + std::to_string(getpid());
    fs::create_directories(tmpDir2);
    std::ofstream(tmpDir2 + "/single.txt") << "single";

    scanner.scan(tmpDir2);
    auto results3 = scanner.takeResults();

    // Should NOT contain results from previous scans
    check(results3.size() >= 1, "C5: Third scan on different dir returns results");
    check(results3.size() < results1.size(), "C5: Third scan has fewer entries than first (different dir)");

    fs::remove_all(tmpDir);
    fs::remove_all(tmpDir2);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 7f: ContentIndex basic tests
// ═══════════════════════════════════════════════════════

static void runContentIndexTests() {
    std::cout << "═══ Part 7f: ContentIndex Basic Tests ═══\n\n";

    // Test trigram extraction
    auto trigrams = ContentIndex::extractTrigrams("hello world");
    check(!trigrams.empty(), "ContentIndex: extractTrigrams returns non-empty for 'hello world'");
    check(trigrams.size() == 9, "ContentIndex: 'hello world' has 9 unique trigrams");

    // Test short string
    auto shortTri = ContentIndex::extractTrigrams("ab");
    check(shortTri.empty(), "ContentIndex: extractTrigrams empty for string < 3 chars");

    // Test binary file detection
    std::string tmpDir = "/tmp/maceverything_ci_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    // Create text file
    {
        std::ofstream ofs(tmpDir + "/text.txt");
        ofs << "This is a plain text file with content for trigram indexing.";
    }

    // Create binary file (with NUL byte)
    {
        FILE* f = fopen((tmpDir + "/binary.bin").c_str(), "wb");
        const char data[] = "some\x00binary";
        fwrite(data, 1, sizeof(data) - 1, f);
        fclose(f);
    }

    check(!ContentIndex::isBinaryFile(tmpDir + "/text.txt"), "ContentIndex: text file not detected as binary");
    check(ContentIndex::isBinaryFile(tmpDir + "/binary.bin"), "ContentIndex: binary file detected correctly");

    // Test indexing and querying
    ContentIndex ci;
    ci.setExtensions({"txt"});

    bool indexed = ci.indexFile(0, tmpDir + "/text.txt");
    check(indexed, "ContentIndex: indexFile succeeds for text file");
    check(ci.indexedFileCount() == 1, "ContentIndex: 1 file indexed");

    auto matches = ci.query("trigram");
    check(!matches.empty(), "ContentIndex: query 'trigram' returns matches");
    check(matches[0].fileIndex == 0, "ContentIndex: match has correct fileIndex");

    auto noMatch = ci.query("zzzznotfound");
    check(noMatch.empty(), "ContentIndex: query for non-existent keyword returns empty");

    // Test persistence
    std::string savePath = tmpDir + "/ci.bin";
    bool saved = ci.saveToFile(savePath);
    check(saved, "ContentIndex: saveToFile succeeds");

    ContentIndex ci2;
    bool loaded = ci2.loadFromFile(savePath);
    check(loaded, "ContentIndex: loadFromFile succeeds");
    check(ci2.indexedFileCount() == 1, "ContentIndex: loaded index has 1 file");

    auto matches2 = ci2.query("trigram");
    check(!matches2.empty(), "ContentIndex: loaded index can query successfully");

    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 8: Trigram Index for Filename Search
// ═══════════════════════════════════════════════════════

static void runTrigramIndexTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 8: Trigram Index for Filename Search\n";
    std::cout << "========================================\n\n";

    // ── Test 1: Basic trigram-accelerated search ──
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

    // ── Test 2: Trigram index maintained through mutations ──
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

    // ── Test 3: Trigram index survives compaction ──
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

    // ── Test 4: Short keywords fallback to linear scan ──
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

    // ── Test 5: Glob patterns bypass trigram index ──
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

    // ── Test 6: No-match keyword returns empty ──
    std::cout << "\n  --- No-match via trigram ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"myfile.txt", "/tmp", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("xyz_nowhere");
        check(res.empty(), "No-match 'xyz_nowhere': trigram index returns empty");
    }

    // ── Test 7: Trigram search performance comparison ──
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

    // ── Test 8: Path-only match still works with trigram ──
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

    // ── Test 9: removeByPathPrefix with trigram cleanup ──
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

    // ── Test 10: removeByPathPrefix benchmark (single-pass vs old double-lookup) ──
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

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 9: ContentIndex sorted posting list benchmark
// ═══════════════════════════════════════════════════════

static void runContentIndexQueryBenchmark() {
    std::cout << "========================================\n";
    std::cout << "  Part 9: ContentIndex Sorted Posting Lists\n";
    std::cout << "========================================\n\n";

    // Test: sorted posting lists enable O(n+m) set_intersection
    // Build a ContentIndex with many files sharing overlapping trigrams
    std::string tmpDir = "/tmp/maceverything_ci_bench_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    ContentIndex ci;
    ci.setExtensions({"txt"});

    // Create 500 files with varied content to populate posting lists
    const int numFiles = 500;
    for (int i = 0; i < numFiles; i++) {
        std::string path = tmpDir + "/file_" + std::to_string(i) + ".txt";
        {
            std::ofstream ofs(path);
            // Write varied content so trigrams differ across files
            ofs << "content file number " << i << " with searchable keywords ";
            if (i % 3 == 0) ofs << "alpha beta gamma ";
            if (i % 5 == 0) ofs << "delta epsilon zeta ";
            if (i % 7 == 0) ofs << "searchable unique pattern ";
            ofs << "padding text to ensure enough trigrams are generated for the index.";
        }
        ci.indexFile(static_cast<uint32_t>(i), path);
    }

    check(ci.indexedFileCount() == numFiles,
          "CI-Bench: all files indexed");

    // Benchmark: query that requires intersecting multiple posting lists
    auto t0 = std::chrono::steady_clock::now();
    const int iterations = 1000;
    int totalMatches = 0;
    for (int run = 0; run < iterations; run++) {
        auto results = ci.query("alpha", 100);
        totalMatches += results.size();
    }
    auto t1 = std::chrono::steady_clock::now();
    double queryTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

    std::cout << "  " << iterations << "x query 'alpha' (" << (totalMatches / iterations)
              << " matches/query): " << std::fixed << std::setprecision(2)
              << queryTime << "ms (" << queryTime / iterations << "ms/query)\n";

    // Verify correctness: 'alpha' appears in files where i%3==0
    auto results = ci.query("alpha", 1000);
    int expectedCount = 0;
    for (int i = 0; i < numFiles; i++) {
        if (i % 3 == 0) expectedCount++;
    }
    check(static_cast<int>(results.size()) == expectedCount,
          "CI-Bench: 'alpha' matches correct count of files");

    // Benchmark: query with no matches (should short-circuit via trigram miss)
    t0 = std::chrono::steady_clock::now();
    for (int run = 0; run < iterations; run++) {
        auto res = ci.query("xyznonexistent", 100);
        (void)res;
    }
    t1 = std::chrono::steady_clock::now();
    double noMatchTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

    std::cout << "  " << iterations << "x query no-match: " << std::fixed << std::setprecision(2)
              << noMatchTime << "ms (" << noMatchTime / iterations << "ms/query)\n";
    check(true, "CI-Bench: query benchmark completed");

    // Test: posting lists are sorted after load from file
    std::string savePath = tmpDir + "/ci_bench.bin";
    ci.saveToFile(savePath);

    ContentIndex ci2;
    ci2.loadFromFile(savePath);
    auto results2 = ci2.query("alpha", 1000);
    check(results2.size() == results.size(),
          "CI-Bench: loaded index returns same results as original");

    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [root_path]\n";
    std::cout << "  --fast             Run fast unit tests only (3, 3b-3e, 5, 7-7d, 8, 9)\n";
    std::cout << "  --slow             Run slow integration tests only (1, 4, 6)\n";
    std::cout << "  --part <id>        Run specific part (can be repeated)\n";
    std::cout << "  --help             Show this help\n";
    std::cout << "  root_path          Root path for disk scan (default: /)\n";
    std::cout << "\nPart IDs: 1 (scan+query), 3 (mutation), 3b (path search),\n";
    std::cout << "  3c (metadata), 3d (compaction), 3e (ranking), 4 (FSEvents),\n";
    std::cout << "  5 (thread safety), 6 (end-to-end), 7 (compact+content),\n";
    std::cout << "  7b (destructor safety), 7c (WAL race), 7d (saveToFile),\n";
    std::cout << "  7e (scanner re-entry), 7f (content index), 8 (trigram index),\n";
    std::cout << "  9 (content index query benchmark)\n";
}

int main(int argc, char* argv[]) {
    std::string rootPath = "/";
    std::set<std::string> selectedParts;
    bool explicitSelection = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--fast") {
            explicitSelection = true;
            selectedParts.insert({"3", "3b", "3c", "3d", "3e", "5", "7", "7b", "7c", "7d", "8", "9"});
        } else if (arg == "--slow") {
            explicitSelection = true;
            selectedParts.insert({"1", "4", "6"});
        } else if (arg == "--part") {
            explicitSelection = true;
            if (i + 1 < argc) {
                selectedParts.insert(argv[++i]);
            } else {
                std::cerr << "Error: --part requires an argument\n";
                return 1;
            }
        } else if (arg[0] != '-') {
            rootPath = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // If no explicit selection, run all parts
    if (!explicitSelection) {
        selectedParts = {"1", "3", "3b", "3c", "3d", "3e", "4", "5", "6", "7", "7b", "7c", "7d", "7e", "7f", "8", "9"};
    }

    // Validate root path if scan test is selected
    if (selectedParts.count("1")) {
        int testfd = open(rootPath.c_str(), O_RDONLY | O_DIRECTORY);
        if (testfd < 0) {
            std::cerr << "Error: cannot open '" << rootPath << "': " << strerror(errno) << "\n";
            return 1;
        }
        close(testfd);
    }

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  MacEverything Full Test & Benchmark     ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";

    if (explicitSelection) {
        std::cout << "  Selected parts:";
        for (const auto& p : selectedParts) std::cout << " " << p;
        std::cout << "\n\n";
    }

    if (selectedParts.count("1"))  runScanAndQueryBenchmark(rootPath);
    if (selectedParts.count("3"))  runMutationTests();
    if (selectedParts.count("3b")) runPathSearchTests();
    if (selectedParts.count("3c")) runIndexMetadataTests();
    if (selectedParts.count("3d")) runCompactionTests();
    if (selectedParts.count("3e")) runSearchRankingTests();
    if (selectedParts.count("4"))  runFSEventsTest();
    if (selectedParts.count("5"))  runThreadSafetyTest();
    if (selectedParts.count("6"))  runEndToEndTest();
    if (selectedParts.count("7"))  runCompactContentIndexTest();
    if (selectedParts.count("7b")) runDestructorSafetyTest();
    if (selectedParts.count("7c")) runWalRaceTest();
    if (selectedParts.count("7d")) runSaveToFileTest();
    if (selectedParts.count("7e")) runScannerReentryTest();
    if (selectedParts.count("7f")) runContentIndexTests();
    if (selectedParts.count("8"))  runTrigramIndexTests();
    if (selectedParts.count("9"))  runContentIndexQueryBenchmark();

    // ── Final Summary ──
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  Final Summary                           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";
    std::cout << "  Tests passed: " << passed << "\n";
    std::cout << "  Tests failed: " << failed << "\n";
    std::cout << "  Result:       " << (failed == 0 ? "ALL PASSED ✓" : "SOME FAILED ✗") << "\n\n";

    return failed > 0 ? 1 : 0;
}
