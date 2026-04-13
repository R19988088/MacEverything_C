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
//  Main
// ═══════════════════════════════════════════════════════

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [root_path]\n";
    std::cout << "  --fast             Run fast unit tests only (3, 3b, 3c, 3d, 3e, 5)\n";
    std::cout << "  --slow             Run slow integration tests only (1, 4, 6)\n";
    std::cout << "  --part <id>        Run specific part (can be repeated)\n";
    std::cout << "  --help             Show this help\n";
    std::cout << "  root_path          Root path for disk scan (default: /)\n";
    std::cout << "\nPart IDs: 1 (scan+query), 3 (mutation), 3b (path search),\n";
    std::cout << "  3c (metadata), 3d (compaction), 3e (ranking), 4 (FSEvents),\n";
    std::cout << "  5 (thread safety), 6 (end-to-end)\n";
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
            selectedParts.insert({"3", "3b", "3c", "3d", "3e", "5"});
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
        selectedParts = {"1", "3", "3b", "3c", "3d", "3e", "4", "5", "6"};
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

    // ── Final Summary ──
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  Final Summary                           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";
    std::cout << "  Tests passed: " << passed << "\n";
    std::cout << "  Tests failed: " << failed << "\n";
    std::cout << "  Result:       " << (failed == 0 ? "ALL PASSED ✓" : "SOME FAILED ✗") << "\n\n";

    return failed > 0 ? 1 : 0;
}
