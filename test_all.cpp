// MacEverything — Full Test Suite + Performance Report
// Compile: clang++ -std=c++20 -O2 -framework CoreServices MacEverything/Core/*.cpp test_all.cpp -o test_all
// Run:     ./test_all [root_path]            (all tests, default root: /)
//          ./test_all --fast                 (fast unit tests only: 3, 3b-3e, 5, 7-14, 15)
//          ./test_all --slow [root_path]     (slow integration tests: 1, 4, 6)
//          ./test_all --part 3 --part 3b     (specific parts)

#include "MacEverything/Core/DirectoryScanner.h"
#include "MacEverything/Core/SearchEngine.h"
#include "MacEverything/Core/ContentIndex.h"
#include "MacEverything/Core/IndexPersistence.h"
#include "MacEverything/Core/ContentIndexPersistence.h"
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
#include <dispatch/dispatch.h>

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
#include "tests/test_wal_crc.h"
#include "tests/test_compact_content.h"
#include "tests/test_destructor_safety.h"
#include "tests/test_wal_race.h"
#include "tests/test_save_file.h"
#include "tests/test_scanner_reentry.h"
#include "tests/test_content_index.h"
#include "tests/test_trigram_index.h"
#include "tests/test_content_query_bench.h"
#include "tests/test_wal_batch_fsync.h"
#include "tests/test_recent_indices.h"
#include "tests/test_parallel_snippets.h"
#include "tests/test_scanner_cancel.h"
#include "tests/test_wal_replay_timeout.h"
#include "tests/test_recent_cache.h"
#include "tests/test_query_cancel.h"
#include "tests/test_critical_high.h"
#include "tests/test_rapid_typing.h"

// ═══════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [root_path]\n";
    std::cout << "  --fast             Run fast unit tests only (3, 3b-3e, 5, 7-7d, 8, 9, 13, 14)\n";
    std::cout << "  --slow             Run slow integration tests only (1, 4, 6)\n";
    std::cout << "  --part <id>        Run specific part (can be repeated)\n";
    std::cout << "  --help             Show this help\n";
    std::cout << "  root_path          Root path for disk scan (default: /)\n";
    std::cout << "\nPart IDs: 1 (scan+query), 3 (mutation), 3b (path search),\n";
    std::cout << "  3c (metadata), 3d (compaction), 3e (ranking), 4 (FSEvents),\n";
    std::cout << "  5 (thread safety), 6 (end-to-end), 7 (compact+content),\n";
    std::cout << "  7b (destructor safety), 7c (WAL race), 7d (saveToFile),\n";
    std::cout << "  7e (scanner re-entry), 7f (content index), 8 (trigram index),\n";
    std::cout << "  9 (content index query benchmark), 10 (WAL batch fsync),\n";
    std::cout << "  11 (recentIndices), 12 (parallel snippets),\n";
    std::cout << "  13 (scanner cancel), 14 (WAL replay timeout),\n";
    std::cout << "  15 (WAL CRC), 16 (recent cache), 17 (query cancel), 18 (critical/high fixes),\n";
    std::cout << "  19 (rapid typing)\n";
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
            selectedParts.insert({"3", "3b", "3c", "3d", "3e", "5", "7", "7b", "7c", "7d", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19"});
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
        selectedParts = {"1", "3", "3b", "3c", "3d", "3e", "4", "5", "6", "7", "7b", "7c", "7d", "7e", "7f", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19"};
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
    if (selectedParts.count("10")) runWalBatchFsyncBenchmark();
    if (selectedParts.count("11")) runRecentIndicesTests();
    if (selectedParts.count("12")) runParallelSnippetTests();
    if (selectedParts.count("13")) runScannerCancelTest();
    if (selectedParts.count("14")) runWalReplayTimeoutTest();
    if (selectedParts.count("15")) runWalCrcTests();
    if (selectedParts.count("16")) runRecentCacheTests();
    if (selectedParts.count("17")) runQueryCancelTests();
    if (selectedParts.count("18")) runCriticalHighTests();
    if (selectedParts.count("19")) runRapidTypingTest();

    // ── Final Summary ──
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  Final Summary                           ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";
    std::cout << "  Tests passed: " << passed << "\n";
    std::cout << "  Tests failed: " << failed << "\n";
    std::cout << "  Result:       " << (failed == 0 ? "ALL PASSED ✓" : "SOME FAILED ✗") << "\n\n";

    return failed > 0 ? 1 : 0;
}
