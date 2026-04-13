// MacEverything — Full Test Suite + Performance Report
// Compile: clang++ -std=c++20 -O2 -framework CoreServices MacEverything/Core/*.cpp test_all.cpp -o test_all
// Run:     ./test_all [root_path]            (all tests, default root: /)
//          ./test_all --fast                 (fast unit tests only: 3, 3b, 3c, 3d, 3e, 5)
//          ./test_all --slow [root_path]     (slow integration tests: 1, 4, 6)
//          ./test_all --part 3 --part 3b     (specific parts)

#include "MacEverything/Core/DirectoryScanner.h"
#include "MacEverything/Core/SearchEngine.h"
#include "MacEverything/Core/FileSystemWatcher.h"
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

// ─────────── Helpers ───────────

static size_t getMemoryUsageMB() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size / (1024 * 1024);
    }
    return 0;
}

static int passed = 0, failed = 0;

static void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "    [PASS] " << msg << "\n";
        passed++;
    } else {
        std::cout << "    [FAIL] " << msg << "\n";
        failed++;
    }
}

// ═══════════════════════════════════════════════════════
//  Part 1 + 2: Full Disk Scan + Query Benchmark
// ═══════════════════════════════════════════════════════

static SearchEngine globalEngine;

static void runScanAndQueryBenchmark(const std::string& rootPath) {
    size_t memBefore = getMemoryUsageMB();

    std::cout << "========================================\n";
    std::cout << "  Part 1: Full Disk Scan\n";
    std::cout << "========================================\n\n";
    std::cout << "  Root path: " << rootPath << "\n";
    std::cout << "  Memory before scan: " << memBefore << " MB\n\n";

    auto t0 = std::chrono::steady_clock::now();
    DirectoryScanner scanner;
    scanner.scan(rootPath);
    auto t1 = std::chrono::steady_clock::now();
    double scanTime = std::chrono::duration<double>(t1 - t0).count();

    const auto& stats = scanner.getStats();
    uint64_t files = stats.fileCount.load();
    uint64_t dirs = stats.dirCount.load();
    uint64_t symlinks = stats.symlinkCount.load();
    uint64_t others = stats.otherCount.load();
    uint64_t errors = stats.errorCount.load();
    uint64_t total = files + dirs + symlinks + others;

    size_t memAfterScan = getMemoryUsageMB();

    std::cout << "  Files:       " << files << "\n";
    std::cout << "  Directories: " << dirs << "\n";
    std::cout << "  Symlinks:    " << symlinks << "\n";
    std::cout << "  Other:       " << others << "\n";
    std::cout << "  Errors:      " << errors << "\n";
    std::cout << "  Total:       " << total << "\n";
    std::cout << "  Scan time:   " << std::fixed << std::setprecision(3) << scanTime << "s\n";
    std::cout << "  Throughput:  " << std::fixed << std::setprecision(0) << (total / scanTime) << " entries/s\n";
    std::cout << "  Memory:      " << memAfterScan << " MB\n\n";

    // Build index
    std::cout << "  Building Search Index...\n";
    auto results = scanner.takeResults();
    auto t2 = std::chrono::steady_clock::now();
    globalEngine.loadRecords(std::move(results));
    auto t3 = std::chrono::steady_clock::now();
    double indexTime = std::chrono::duration<double>(t3 - t2).count();

    size_t memAfterIndex = getMemoryUsageMB();
    std::cout << "  Records:     " << globalEngine.recordCount() << "\n";
    std::cout << "  Live:        " << globalEngine.liveRecordCount() << "\n";
    std::cout << "  Index time:  " << std::fixed << std::setprecision(3) << indexTime << "s\n";
    std::cout << "  Memory:      " << memAfterIndex << " MB\n";
    std::cout << "  Per-entry:   ~" << std::setprecision(0) << (memAfterIndex * 1024.0 * 1024.0 / total) << " bytes\n";
    std::cout << "  Total startup: " << std::fixed << std::setprecision(3) << (scanTime + indexTime) << "s\n\n";

    // ── Part 2: Query Performance ──
    std::cout << "========================================\n";
    std::cout << "  Part 2: Query Performance\n";
    std::cout << "========================================\n\n";

    struct QueryTest {
        std::string keyword;
        uint32_t maxResults;
        std::string description;
    };

    std::vector<QueryTest> tests = {
        {"README",       0,     "Common filename (README)"},
        {"readme",       0,     "Case-insensitive (readme)"},
        {".cpp",         0,     "File extension (.cpp)"},
        {".swift",       0,     "File extension (.swift)"},
        {".json",        0,     "File extension (.json)"},
        {".py",          0,     "File extension (.py)"},
        {"node_modules", 0,     "Directory name (node_modules)"},
        {"index",        0,     "Common name (index)"},
        {"test",         0,     "Common name (test)"},
        {"a",            0,     "Single char (a) - many matches"},
        {"xyzxyzxyz",    0,     "No match (xyzxyzxyz)"},
        {".cpp",         100,   ".cpp maxResults=100"},
        {".cpp",         1000,  ".cpp maxResults=1000"},
        {"test",         50000, "test maxResults=50000"},
    };

    // Warm up
    globalEngine.query("warmup", 10);

    std::cout << std::left << std::setw(40) << "  Query"
              << std::right << std::setw(10) << "Matches"
              << std::setw(12) << "Time(ms)" << "\n";
    std::cout << "  " << std::string(60, '-') << "\n";

    double totalQueryTime = 0;
    int unlimitedCount = 0;

    for (const auto& test : tests) {
        double bestTime = 1e9;
        size_t matchCount = 0;

        for (int run = 0; run < 3; run++) {
            auto qs = std::chrono::steady_clock::now();
            auto indices = globalEngine.query(test.keyword, test.maxResults);
            auto qe = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(qe - qs).count() * 1000;
            if (elapsed < bestTime) bestTime = elapsed;
            matchCount = indices.size();
        }

        std::cout << "  " << std::left << std::setw(40) << test.description
                  << std::right << std::setw(10) << matchCount
                  << std::setw(10) << std::fixed << std::setprecision(2) << bestTime << "ms\n";

        if (test.maxResults == 0) {
            totalQueryTime += bestTime;
            unlimitedCount++;
        }
    }

    if (unlimitedCount > 0) {
        std::cout << "\n  Avg query time (unlimited): "
                  << std::fixed << std::setprecision(2) << (totalQueryTime / unlimitedCount) << "ms\n";
    }

    // Record fetch
    std::cout << "\n  Record Fetch Benchmark:\n";
    auto fetchIndices = globalEngine.query("a", 10000);
    auto f0 = std::chrono::steady_clock::now();
    for (uint32_t idx : fetchIndices) {
        const auto& r = globalEngine.getRecord(idx);
        (void)r.name;
    }
    auto f1 = std::chrono::steady_clock::now();
    double fetchTime = std::chrono::duration<double>(f1 - f0).count() * 1000;
    std::cout << "  Fetched " << fetchIndices.size() << " records in "
              << std::fixed << std::setprecision(2) << fetchTime << "ms"
              << " (" << std::setprecision(1) << (fetchTime / fetchIndices.size() * 1000) << "us/record)\n\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3: SearchEngine Mutation Tests
// ═══════════════════════════════════════════════════════

static void runMutationTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3: SearchEngine Mutation Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;

    // Load some initial records
    std::vector<FileRecord> initial;
    initial.push_back({"hello.txt", "/tmp", 1, 100, 1000});
    initial.push_back({"world.cpp", "/tmp", 1, 200, 2000});
    initial.push_back({"readme.md", "/home", 1, 300, 3000});
    initial.push_back({"testdir", "/var", 2, 0, 4000});
    engine.loadRecords(std::move(initial));

    check(engine.recordCount() == 4, "loadRecords: recordCount == 4");
    check(engine.liveRecordCount() == 4, "loadRecords: liveRecordCount == 4");

    // Query test
    auto res = engine.query("hello");
    check(res.size() == 1, "query 'hello': 1 match");
    check(engine.getRecord(res[0]).name == "hello.txt", "query 'hello': correct name");

    res = engine.query(".txt");
    check(res.size() == 1, "query '.txt': 1 match");

    // Case insensitive
    res = engine.query("README");
    check(res.size() == 1, "query 'README' (case-insensitive): 1 match");

    // ── addRecord ──
    std::cout << "\n  --- addRecord ---\n";
    uint32_t newIdx = engine.addRecord({"newfile.txt", "/tmp", 1, 500, 5000});
    check(newIdx == 4, "addRecord: returned index 4");
    check(engine.recordCount() == 5, "addRecord: recordCount == 5");
    check(engine.liveRecordCount() == 5, "addRecord: liveRecordCount == 5");
    res = engine.query("newfile");
    check(res.size() == 1, "addRecord: queryable by name");
    check(engine.getRecord(res[0]).size == 500, "addRecord: correct size");

    // ── removeByPath ──
    std::cout << "\n  --- removeByPath ---\n";
    bool removed = engine.removeByPath("/tmp/hello.txt");
    check(removed, "removeByPath '/tmp/hello.txt': returned true");
    check(engine.liveRecordCount() == 4, "removeByPath: liveRecordCount == 4");
    res = engine.query("hello");
    check(res.empty(), "removeByPath: 'hello' no longer queryable");

    // Tombstoned record
    const auto& tombstoned = engine.getRecord(0);
    check(tombstoned.type == 0, "removeByPath: record type == 0 (tombstone)");

    // Remove non-existent
    bool removedAgain = engine.removeByPath("/tmp/hello.txt");
    check(!removedAgain, "removeByPath non-existent: returned false");
    check(engine.liveRecordCount() == 4, "removeByPath non-existent: liveCount unchanged");

    bool removedBogus = engine.removeByPath("/no/such/path");
    check(!removedBogus, "removeByPath bogus path: returned false");

    // ── updateByPath ──
    std::cout << "\n  --- updateByPath ---\n";
    engine.updateByPath("/tmp/world.cpp", {"world_v2.cpp", "/tmp", 1, 999, 6000});
    check(engine.liveRecordCount() == 4, "updateByPath: liveRecordCount unchanged (4)");
    res = engine.query("world_v2");
    check(res.size() == 1, "updateByPath: new name queryable");
    check(engine.getRecord(res[0]).size == 999, "updateByPath: new record has updated size");
    res = engine.query("world.cpp");
    // world.cpp is tombstoned but world_v2.cpp contains "world" substring
    // The old exact "world.cpp" record is gone, let's check the tombstone
    const auto& oldRecord = engine.getRecord(1);
    check(oldRecord.type == 0, "updateByPath: old record tombstoned");

    // Update non-existent path (should just add)
    engine.updateByPath("/new/path/fresh.go", {"fresh.go", "/new/path", 1, 42, 7000});
    check(engine.liveRecordCount() == 5, "updateByPath new path: liveRecordCount == 5");
    res = engine.query("fresh.go");
    check(res.size() == 1, "updateByPath new path: queryable");

    // ── Mutation performance ──
    std::cout << "\n  --- Mutation Performance ---\n";
    SearchEngine perfEngine;
    {
        std::vector<FileRecord> bulk;
        for (uint32_t i = 0; i < 100000; i++) {
            bulk.push_back({"file_" + std::to_string(i) + ".txt", "/perf/dir", 1, i * 10, (time_t)i});
        }
        perfEngine.loadRecords(std::move(bulk));
    }

    // Benchmark addRecord
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i++) {
        perfEngine.addRecord({"added_" + std::to_string(i) + ".txt", "/perf/add", 1, 100, 1000});
    }
    auto t1 = std::chrono::steady_clock::now();
    double addTime = std::chrono::duration<double>(t1 - t0).count() * 1000;
    std::cout << "    10,000 addRecord:    " << std::fixed << std::setprecision(2) << addTime << "ms"
              << " (" << std::setprecision(1) << (addTime / 10000 * 1000) << "us/op)\n";

    // Benchmark removeByPath
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i++) {
        perfEngine.removeByPath("/perf/dir/file_" + std::to_string(i) + ".txt");
    }
    t1 = std::chrono::steady_clock::now();
    double removeTime = std::chrono::duration<double>(t1 - t0).count() * 1000;
    std::cout << "    10,000 removeByPath: " << std::fixed << std::setprecision(2) << removeTime << "ms"
              << " (" << std::setprecision(1) << (removeTime / 10000 * 1000) << "us/op)\n";

    check(perfEngine.liveRecordCount() == 100000, "Mutation perf: liveCount == 100000 (100k - 10k removed + 10k added)");

    // Benchmark updateByPath
    t0 = std::chrono::steady_clock::now();
    for (int i = 10000; i < 20000; i++) {
        perfEngine.updateByPath("/perf/dir/file_" + std::to_string(i) + ".txt",
                                {"updated_" + std::to_string(i) + ".txt", "/perf/dir", 1, 999, 9999});
    }
    t1 = std::chrono::steady_clock::now();
    double updateTime = std::chrono::duration<double>(t1 - t0).count() * 1000;
    std::cout << "    10,000 updateByPath: " << std::fixed << std::setprecision(2) << updateTime << "ms"
              << " (" << std::setprecision(1) << (updateTime / 10000 * 1000) << "us/op)\n";

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3b: Path-based Search Tests
// ═══════════════════════════════════════════════════════

static void runPathSearchTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3b: Path-based Search Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;

    std::vector<FileRecord> records;
    records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
    records.push_back({"config.json", "/etc/myapp", 1, 200, 2000});
    records.push_back({"readme.md", "/home/user/projects", 1, 300, 3000});
    records.push_back({"libfoo.dylib", "/usr/local/lib", 1, 400, 4000});
    records.push_back({"include", "/usr/local", 2, 0, 5000});
    engine.loadRecords(std::move(records));

    // Search by path substring
    auto res = engine.query("/usr/local");
    check(res.size() == 3, "Path search '/usr/local': 3 matches (src/main.cpp, lib/libfoo.dylib, include)");

    res = engine.query("/etc");
    check(res.size() == 1, "Path search '/etc': 1 match");
    check(engine.getRecord(res[0]).name == "config.json", "Path search '/etc': correct file");

    // Search by directory name only
    res = engine.query("projects");
    check(res.size() == 1, "Dir name search 'projects': 1 match");
    check(engine.getRecord(res[0]).name == "readme.md", "Dir name search 'projects': correct file");

    // Search that matches both name and path
    res = engine.query("local");
    check(res.size() == 3, "Search 'local': 3 matches (path contains '/usr/local')");

    // Glob on path
    res = engine.query("*/lib/*");
    check(res.size() == 1, "Glob '*/lib/*': 1 match");
    check(engine.getRecord(res[0]).name == "libfoo.dylib", "Glob '*/lib/*': correct file");

    // Path search after addRecord
    engine.addRecord({"newlib.a", "/usr/local/lib", 1, 500, 6000});
    res = engine.query("/usr/local/lib");
    check(res.size() == 2, "After addRecord: '/usr/local/lib' returns 2 matches");

    // Path search after removeByPath
    engine.removeByPath("/usr/local/lib/libfoo.dylib");
    res = engine.query("/usr/local/lib");
    check(res.size() == 1, "After removeByPath: '/usr/local/lib' returns 1 match");

    // Path search after updateByPath
    engine.updateByPath("/usr/local/lib/newlib.a", {"newlib_v2.a", "/opt/lib", 1, 600, 7000});
    res = engine.query("/usr/local/lib");
    check(res.size() == 0, "After updateByPath: '/usr/local/lib' returns 0 matches");
    res = engine.query("/opt/lib");
    check(res.size() == 1, "After updateByPath: '/opt/lib' returns 1 match");

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3c: Index Metadata & Version Tests
// ═══════════════════════════════════════════════════════

static void runIndexMetadataTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3c: Index Metadata & Version Tests\n";
    std::cout << "========================================\n\n";

    std::string tmpFile = "/tmp/maceverything_meta_test_" + std::to_string(getpid()) + ".bin";

    // ── Test 1: Save and load v3 with metadata ──
    std::cout << "  --- Save/Load v3 with metadata ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"file1.txt", "/tmp", 1, 100, 1000, 12345, 1});
        records.push_back({"file2.cpp", "/src", 1, 200, 2000, 67890, 1});
        records.push_back({"mydir", "/var", 2, 0, 3000, 11111, 2});
        engine.loadRecords(std::move(records));

        IndexMetadata meta;
        meta.lastEventId = 42;
        meta.extra[IndexMetadata::kScanRoot] = "/";
        meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
        meta.extra[IndexMetadata::kOSVersion] = "14.5.0";
        meta.extra[IndexMetadata::kRecordFormat] = "v3_inode";
        meta.extra["custom_key"] = "custom_value";

        bool saved = engine.saveToFile(tmpFile, meta);
        check(saved, "v3 saveToFile succeeded");
    }

    {
        SearchEngine engine;
        IndexMetadata loadedMeta;
        bool loaded = engine.loadFromFile(tmpFile, &loadedMeta);
        check(loaded, "v3 loadFromFile succeeded");
        check(loadedMeta.formatVersion == 3, "v3: formatVersion == 3");
        check(loadedMeta.lastEventId == 42, "v3: lastEventId == 42");
        check(loadedMeta.timestamp > 0, "v3: timestamp > 0");
        check(engine.liveRecordCount() == 3, "v3: 3 live records");

        // Check metadata key-value pairs
        check(loadedMeta.extra.count(IndexMetadata::kScanRoot) == 1, "v3: has scan_root key");
        check(loadedMeta.extra[IndexMetadata::kScanRoot] == "/", "v3: scan_root == '/'");
        check(loadedMeta.extra[IndexMetadata::kAppVersion] == "1.1.0", "v3: app_version == '1.1.0'");
        check(loadedMeta.extra[IndexMetadata::kOSVersion] == "14.5.0", "v3: os_version == '14.5.0'");
        check(loadedMeta.extra[IndexMetadata::kRecordFormat] == "v3_inode", "v3: record_format == 'v3_inode'");
        check(loadedMeta.extra["custom_key"] == "custom_value", "v3: custom_key preserved");

        // Verify record data integrity
        auto res = engine.query("file1");
        check(res.size() == 1, "v3 load: file1 queryable");
        const auto& r = engine.getRecord(res[0]);
        check(r.inode == 12345, "v3 load: inode preserved");
        check(r.devId == 1, "v3 load: devId preserved");
    }

    // ── Test 2: Legacy overload (uint64_t lastEventId) still works ──
    std::cout << "\n  --- Legacy overload compatibility ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"legacy.txt", "/old", 1, 50, 500});
        engine.loadRecords(std::move(records));

        bool saved = engine.saveToFile(tmpFile, (uint64_t)99);
        check(saved, "Legacy save overload succeeded");
    }

    {
        SearchEngine engine;
        uint64_t lastEventId = 0;
        bool loaded = engine.loadFromFile(tmpFile, &lastEventId);
        check(loaded, "Legacy load overload succeeded");
        check(lastEventId == 99, "Legacy load: lastEventId == 99");
        check(engine.liveRecordCount() == 1, "Legacy load: 1 record");
    }

    // ── Test 3: Empty metadata ──
    std::cout << "\n  --- Empty metadata ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"empty_meta.txt", "/test", 1, 10, 100});
        engine.loadRecords(std::move(records));

        IndexMetadata meta;
        meta.lastEventId = 0;
        // No extra metadata
        bool saved = engine.saveToFile(tmpFile, meta);
        check(saved, "Empty metadata save succeeded");
    }

    {
        SearchEngine engine;
        IndexMetadata loadedMeta;
        bool loaded = engine.loadFromFile(tmpFile, &loadedMeta);
        check(loaded, "Empty metadata load succeeded");
        check(loadedMeta.formatVersion == 3, "Empty metadata: formatVersion == 3");
        check(loadedMeta.extra.empty(), "Empty metadata: no extra keys");
    }

    // ── Test 4: Unknown version rejection ──
    std::cout << "\n  --- Unknown version rejection ---\n";
    {
        // Write a file with version 99
        FILE* f = fopen(tmpFile.c_str(), "wb");
        char magic[4] = {'M', 'E', 'I', 'D'};
        fwrite(magic, 1, 4, f);
        uint32_t badVersion = 99;
        fwrite(&badVersion, sizeof(uint32_t), 1, f);
        fclose(f);

        SearchEngine engine;
        bool loaded = engine.loadFromFile(tmpFile);
        check(!loaded, "Unknown version 99: loadFromFile returns false");
    }

    // Cleanup
    fs::remove(tmpFile);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3d: Compact Records Tests
// ═══════════════════════════════════════════════════════

static void runCompactionTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3d: Compact Records Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;
    std::vector<FileRecord> records;
    for (int i = 0; i < 100; i++) {
        records.push_back({"compact_" + std::to_string(i) + ".txt", "/test", 1, (uint64_t)i, (time_t)i});
    }
    engine.loadRecords(std::move(records));

    check(engine.recordCount() == 100, "Compact: initial recordCount == 100");
    check(engine.liveRecordCount() == 100, "Compact: initial liveRecordCount == 100");

    // Remove half the records
    for (int i = 0; i < 50; i++) {
        engine.removeByPath("/test/compact_" + std::to_string(i) + ".txt");
    }
    check(engine.liveRecordCount() == 50, "Compact: after removing 50, liveRecordCount == 50");
    check(engine.recordCount() == 100, "Compact: after removing 50, recordCount still 100 (tombstones)");

    // Compact
    engine.compactRecords();
    check(engine.recordCount() == 50, "Compact: after compaction, recordCount == 50");
    check(engine.liveRecordCount() == 50, "Compact: after compaction, liveRecordCount == 50");

    // Verify remaining records are still queryable
    auto res = engine.query("compact_50");
    check(res.size() == 1, "Compact: compact_50.txt still queryable after compaction");
    check(engine.getRecord(res[0]).name == "compact_50.txt", "Compact: correct record data after compaction");

    // Verify removed records are gone
    res = engine.query("compact_0");
    check(res.empty(), "Compact: compact_0.txt not queryable after compaction");

    // Compact when nothing to compact
    engine.compactRecords();
    check(engine.recordCount() == 50, "Compact: no-op compaction preserves recordCount");

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3e: Search Ranking Tests
// ═══════════════════════════════════════════════════════

static void runSearchRankingTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3e: Search Ranking Tests\n";
    std::cout << "========================================\n\n";

    // ── Test 1: Exact name match gets priority 0 ──
    std::cout << "  --- Exact name match priority ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"Alfred 5.app", "/Applications", 5, 0, 1000});
        records.push_back({"Info.plist", "/Applications/Alfred 5.app/Contents", 1, 100, 2000});
        records.push_back({"Alfred", "/Applications/Alfred 5.app/Contents/MacOS", 1, 5000, 3000});
        records.push_back({"alfred_helper", "/usr/local/bin", 1, 200, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("Alfred 5.app");
        check(!res.empty(), "Ranking: 'Alfred 5.app' has results");
        check(engine.getRecord(res[0]).name == "Alfred 5.app",
              "Ranking: exact name match 'Alfred 5.app' is first result");
    }

    // ── Test 2: Prefix match before contains match ──
    std::cout << "\n  --- Prefix vs contains priority ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // "alfred_helper" starts with "alfred" → priority 1
        // "Alfred 5.app" lowercased is "alfred 5.app", starts with "alfred" → priority 1
        // "xalfred" contains "alfred" but doesn't start with it → priority 2
        records.push_back({"xalfred", "/tmp", 1, 100, 1000});
        records.push_back({"Alfred 5.app", "/Applications", 5, 0, 2000});
        records.push_back({"alfred_helper", "/bin", 1, 200, 3000});  // short path
        records.push_back({"Alfred", "/opt", 1, 5000, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("alfred");
        check(res.size() >= 3, "Ranking: 'alfred' matches multiple records");

        // "Alfred" is exact match (case-insensitive) → priority 0
        check(engine.getRecord(res[0]).name == "Alfred",
              "Ranking: exact match 'Alfred' is first");

        // Both "alfred_helper" and "Alfred 5.app" are prefix matches (priority 1)
        // "alfred_helper" at /bin (path len 18) < "Alfred 5.app" at /Applications (path len 25)
        check(engine.getRecord(res[1]).name == "alfred_helper",
              "Ranking: prefix match 'alfred_helper' is second (shorter path)");

        // "xalfred" contains "alfred" but doesn't start with it → priority 2
    }

    // ── Test 3: Path-only match gets lowest priority ──
    std::cout << "\n  --- Path-only match priority ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
        records.push_back({"local.conf", "/etc", 1, 200, 2000});
        records.push_back({"readme.md", "/usr/local/docs", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("local");
        check(res.size() == 3, "Ranking: 'local' matches 3 records");

        // "local.conf" name contains "local" → priority 2
        check(engine.getRecord(res[0]).name == "local.conf",
              "Ranking: name-match 'local.conf' before path-only matches");

        // "main.cpp" and "readme.md" only match via path → priority 3
        // path lengths: "/usr/local/src/main.cpp" vs "/usr/local/docs/readme.md"
        bool restArePathOnly = true;
        for (size_t i = 1; i < res.size(); i++) {
            auto r = engine.getRecord(res[i]);
            if (r.name.find("local") != std::string::npos) restArePathOnly = false;
        }
        check(restArePathOnly, "Ranking: remaining results are path-only matches");
    }

    // ── Test 4: Same priority sorted by path length ──
    std::cout << "\n  --- Same priority: shorter path first ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"test.txt", "/a/b/c/d/e", 1, 100, 1000});       // deep path
        records.push_back({"test.txt", "/tmp", 1, 100, 2000});              // shallow path
        records.push_back({"test.txt", "/a/b/c", 1, 100, 3000});            // medium path
        engine.loadRecords(std::move(records));

        auto res = engine.query("test.txt");
        check(res.size() == 3, "Ranking: 3 exact matches");

        // All are exact name matches (priority 0), sorted by full path length
        auto r0 = engine.getRecord(res[0]);
        auto r1 = engine.getRecord(res[1]);
        auto r2 = engine.getRecord(res[2]);

        std::string p0 = r0.path + "/" + r0.name;
        std::string p1 = r1.path + "/" + r1.name;
        std::string p2 = r2.path + "/" + r2.name;

        check(p0.size() <= p1.size() && p1.size() <= p2.size(),
              "Ranking: results sorted by ascending path length");
        check(r0.path == "/tmp", "Ranking: shortest path '/tmp/test.txt' is first");
    }

    // ── Test 5: maxResults truncates after sort ──
    std::cout << "\n  --- maxResults truncates after sort ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Add many path-only matches first (high index)
        for (int i = 0; i < 100; i++) {
            records.push_back({"file_" + std::to_string(i) + ".dat",
                               "/deep/nested/target_dir", 1, 100, 1000});
        }
        // Add exact name match last (highest index)
        records.push_back({"target_dir", "/usr", 2, 0, 2000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("target_dir", 5);
        check(!res.empty(), "Ranking: maxResults=5 has results");
        check(engine.getRecord(res[0]).name == "target_dir",
              "Ranking: exact match at high index still first with maxResults=5");
        check(res.size() == 5, "Ranking: maxResults=5 returns exactly 5");
    }

    // ── Test 6: Glob patterns work (no ranking, just match) ──
    std::cout << "\n  --- Glob pattern matching ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"hello.cpp", "/src", 1, 100, 1000});
        records.push_back({"hello.h", "/src", 1, 50, 2000});
        records.push_back({"world.cpp", "/src", 1, 200, 3000});
        records.push_back({"test.py", "/src", 1, 150, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("*.cpp");
        check(res.size() == 2, "Glob '*.cpp': 2 matches");

        res = engine.query("hello.*");
        check(res.size() == 2, "Glob 'hello.*': 2 matches");

        res = engine.query("?orld.cpp");
        check(res.size() == 1, "Glob '?orld.cpp': 1 match");
    }

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 4: FSEvents Integration Test
// ═══════════════════════════════════════════════════════

static void runFSEventsTest() {
    std::cout << "========================================\n";
    std::cout << "  Part 4: FSEvents Integration Test\n";
    std::cout << "========================================\n\n";

    // Create temp directory
    std::string tmpDir = "/tmp/maceverything_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<FileSystemWatcher::Event> receivedEvents;

    FileSystemWatcher watcher;
    watcher.start(tmpDir, [&](std::vector<FileSystemWatcher::Event> events) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& e : events) {
            receivedEvents.push_back(std::move(e));
        }
        cv.notify_all();
    });

    check(watcher.isRunning(), "FileSystemWatcher started");

    // Wait a bit for FSEvents to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto waitForEvents = [&](int expectedMin, int timeoutMs) -> bool {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return (int)receivedEvents.size() >= expectedMin;
        });
    };

    // ── Test 1: Create file ──
    std::cout << "  --- File Create ---\n";
    receivedEvents.clear();
    auto createStart = std::chrono::steady_clock::now();
    {
        std::ofstream ofs(tmpDir + "/testfile.txt");
        ofs << "hello world\n";
    }
    bool gotCreate = waitForEvents(1, 3000);
    auto createEnd = std::chrono::steady_clock::now();
    double createLatency = std::chrono::duration<double>(createEnd - createStart).count() * 1000;

    check(gotCreate, "Create event received");
    if (gotCreate) {
        bool foundPath = false;
        for (const auto& e : receivedEvents) {
            if (e.path.find("testfile.txt") != std::string::npos) {
                foundPath = true;
                break;
            }
        }
        check(foundPath, "Create event has correct path");
        std::cout << "    Event latency: " << std::fixed << std::setprecision(0) << createLatency << "ms\n";
        std::cout << "    Events received: " << receivedEvents.size() << "\n";
    }

    // ── Test 2: Modify file ──
    std::cout << "\n  --- File Modify ---\n";
    receivedEvents.clear();
    auto modStart = std::chrono::steady_clock::now();
    {
        std::ofstream ofs(tmpDir + "/testfile.txt", std::ios::app);
        ofs << "more content\n";
    }
    bool gotModify = waitForEvents(1, 3000);
    auto modEnd = std::chrono::steady_clock::now();
    double modLatency = std::chrono::duration<double>(modEnd - modStart).count() * 1000;

    check(gotModify, "Modify event received");
    if (gotModify) {
        std::cout << "    Event latency: " << std::fixed << std::setprecision(0) << modLatency << "ms\n";
        std::cout << "    Events received: " << receivedEvents.size() << "\n";
    }

    // ── Test 3: Rename file ──
    std::cout << "\n  --- File Rename ---\n";
    receivedEvents.clear();
    auto renStart = std::chrono::steady_clock::now();
    fs::rename(tmpDir + "/testfile.txt", tmpDir + "/renamed.txt");
    bool gotRename = waitForEvents(1, 3000);
    auto renEnd = std::chrono::steady_clock::now();
    double renLatency = std::chrono::duration<double>(renEnd - renStart).count() * 1000;

    check(gotRename, "Rename event received");
    if (gotRename) {
        std::cout << "    Event latency: " << std::fixed << std::setprecision(0) << renLatency << "ms\n";
        std::cout << "    Events received: " << receivedEvents.size() << "\n";
    }

    // ── Test 4: Delete file ──
    std::cout << "\n  --- File Delete ---\n";
    receivedEvents.clear();
    auto delStart = std::chrono::steady_clock::now();
    fs::remove(tmpDir + "/renamed.txt");
    bool gotDelete = waitForEvents(1, 3000);
    auto delEnd = std::chrono::steady_clock::now();
    double delLatency = std::chrono::duration<double>(delEnd - delStart).count() * 1000;

    check(gotDelete, "Delete event received");
    if (gotDelete) {
        std::cout << "    Event latency: " << std::fixed << std::setprecision(0) << delLatency << "ms\n";
        std::cout << "    Events received: " << receivedEvents.size() << "\n";
    }

    // ── Test 5: Batch create ──
    std::cout << "\n  --- Batch Create (100 files) ---\n";
    receivedEvents.clear();
    auto batchStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; i++) {
        std::ofstream ofs(tmpDir + "/batch_" + std::to_string(i) + ".txt");
        ofs << "batch " << i << "\n";
    }
    // Wait for at least some events (FSEvents coalesces)
    bool gotBatch = waitForEvents(10, 5000);
    auto batchEnd = std::chrono::steady_clock::now();
    double batchLatency = std::chrono::duration<double>(batchEnd - batchStart).count() * 1000;

    check(gotBatch, "Batch create: received events");
    std::cout << "    Events received: " << receivedEvents.size() << "\n";
    std::cout << "    Batch latency: " << std::fixed << std::setprecision(0) << batchLatency << "ms\n";

    // Stop watcher
    watcher.stop();
    check(!watcher.isRunning(), "FileSystemWatcher stopped");

    // Cleanup
    fs::remove_all(tmpDir);
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 5: Thread Safety Stress Test
// ═══════════════════════════════════════════════════════

static void runThreadSafetyTest() {
    std::cout << "========================================\n";
    std::cout << "  Part 5: Thread Safety Stress Test\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;

    // Load initial data
    std::vector<FileRecord> initial;
    for (int i = 0; i < 10000; i++) {
        initial.push_back({"stress_" + std::to_string(i) + ".txt", "/stress", 1, (uint64_t)i, (time_t)i});
    }
    engine.loadRecords(std::move(initial));

    std::atomic<bool> running{true};
    std::atomic<uint64_t> readOps{0};
    std::atomic<uint64_t> writeOps{0};
    std::atomic<bool> errorDetected{false};

    constexpr int NUM_READERS = 4;
    constexpr int NUM_WRITERS = 2;
    constexpr int DURATION_SECS = 2;

    // Reader threads
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_READERS; t++) {
        threads.emplace_back([&, t] {
            int queries = 0;
            while (running.load(std::memory_order_relaxed)) {
                try {
                    auto res = engine.query("stress_" + std::to_string(queries % 1000), 100);
                    // Access returned records
                    for (uint32_t idx : res) {
                        const auto& r = engine.getRecord(idx);
                        (void)r.name;
                    }
                    // Also read live count
                    (void)engine.liveRecordCount();
                    readOps.fetch_add(1, std::memory_order_relaxed);
                    queries++;
                } catch (...) {
                    errorDetected.store(true);
                }
            }
        });
    }

    // Writer threads
    for (int t = 0; t < NUM_WRITERS; t++) {
        threads.emplace_back([&, t] {
            int ops = 0;
            while (running.load(std::memory_order_relaxed)) {
                try {
                    std::string name = "w" + std::to_string(t) + "_" + std::to_string(ops) + ".txt";
                    std::string path = "/stress/w" + std::to_string(t);

                    // Add
                    engine.addRecord({name, path, 1, 100, 1000});
                    writeOps.fetch_add(1, std::memory_order_relaxed);

                    // Update
                    engine.updateByPath(path + "/" + name,
                                        {name + ".bak", path, 1, 200, 2000});
                    writeOps.fetch_add(1, std::memory_order_relaxed);

                    // Remove
                    engine.removeByPath(path + "/" + name + ".bak");
                    writeOps.fetch_add(1, std::memory_order_relaxed);

                    ops++;
                } catch (...) {
                    errorDetected.store(true);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SECS));
    running.store(false, std::memory_order_relaxed);

    for (auto& th : threads) {
        th.join();
    }

    uint64_t totalReads = readOps.load();
    uint64_t totalWrites = writeOps.load();

    check(!errorDetected.load(), "No exceptions during concurrent access");
    check(totalReads > 0, "Reader threads performed queries");
    check(totalWrites > 0, "Writer threads performed mutations");

    std::cout << "\n    Duration:     " << DURATION_SECS << "s\n";
    std::cout << "    Readers:      " << NUM_READERS << " threads\n";
    std::cout << "    Writers:      " << NUM_WRITERS << " threads\n";
    std::cout << "    Read ops:     " << totalReads << " (" << totalReads / DURATION_SECS << " ops/s)\n";
    std::cout << "    Write ops:    " << totalWrites << " (" << totalWrites / DURATION_SECS << " ops/s)\n";
    std::cout << "    Total ops:    " << (totalReads + totalWrites) << "\n";
    std::cout << "    Live records: " << engine.liveRecordCount() << "\n\n";
}

// ═══════════════════════════════════════════════════════
//  Part 6: End-to-End Integration Test
//  (FSEvents → SearchEngine pipeline)
// ═══════════════════════════════════════════════════════

static void runEndToEndTest() {
    std::cout << "========================================\n";
    std::cout << "  Part 6: End-to-End Integration\n";
    std::cout << "  (FSEvents -> SearchEngine pipeline)\n";
    std::cout << "========================================\n\n";

    std::string tmpDir = "/tmp/maceverything_e2e_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    SearchEngine engine;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> changeCount{0};

    FileSystemWatcher watcher;
    watcher.start(tmpDir, [&](std::vector<FileSystemWatcher::Event> events) {
        for (const auto& event : events) {
            const std::string& path = event.path;
            FSEventStreamEventFlags flags = event.flags;

            bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
            bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

            struct stat st;
            bool exists = (lstat(path.c_str(), &st) == 0);

            if (itemRemoved || (itemRenamed && !exists)) {
                engine.removeByPath(path);
            } else if (exists) {
                std::string dirPath, fileName;
                size_t lastSlash = path.rfind('/');
                if (lastSlash != std::string::npos) {
                    dirPath = path.substr(0, lastSlash);
                    fileName = path.substr(lastSlash + 1);
                } else {
                    dirPath = ".";
                    fileName = path;
                }
                if (fileName.empty()) continue;

                uint8_t type = 4;
                if (S_ISREG(st.st_mode))     type = 1;
                else if (S_ISDIR(st.st_mode)) type = 2;
                else if (S_ISLNK(st.st_mode)) type = 3;

                FileRecord record;
                record.name = fileName;
                record.path = dirPath;
                record.type = type;
                record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
                record.modTime = st.st_mtime;

                engine.updateByPath(path, std::move(record));
            }
        }
        changeCount.fetch_add(1);
        cv.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto waitForChange = [&](int prevCount, int timeoutMs) -> bool {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return changeCount.load() > prevCount;
        });
    };

    // Create a file
    int prevCount = changeCount.load();
    {
        std::ofstream ofs(tmpDir + "/e2e_test.txt");
        ofs << "e2e test content\n";
    }
    waitForChange(prevCount, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto res = engine.query("e2e_test");
    check(res.size() == 1, "E2E: Created file appears in search");
    if (!res.empty()) {
        const auto& r = engine.getRecord(res[0]);
        check(r.type == 1, "E2E: Record type is file (1)");
        check(r.size > 0, "E2E: Record size > 0");
    }
    check(engine.liveRecordCount() >= 1, "E2E: liveRecordCount >= 1");

    // Rename the file
    prevCount = changeCount.load();
    fs::rename(tmpDir + "/e2e_test.txt", tmpDir + "/e2e_renamed.txt");
    waitForChange(prevCount, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    res = engine.query("e2e_renamed");
    check(res.size() == 1, "E2E: Renamed file appears in search");
    // The old name should be gone (tombstoned)
    res = engine.query("e2e_test");
    // Note: "e2e_test" is a substring of nothing after rename, but "e2e_renamed" does not contain "e2e_test"
    // Actually "e2e_test" won't match "e2e_renamed" since query matches on filename only
    // Let's verify the exact old path is removed
    bool oldRemoved = !engine.removeByPath(tmpDir + "/e2e_test.txt");
    check(oldRemoved, "E2E: Old path no longer in index");

    // Delete the file
    prevCount = changeCount.load();
    fs::remove(tmpDir + "/e2e_renamed.txt");
    waitForChange(prevCount, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    res = engine.query("e2e_renamed");
    check(res.empty(), "E2E: Deleted file removed from search");

    watcher.stop();
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
