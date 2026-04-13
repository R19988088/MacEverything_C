// MacEverything — Full Test Suite + Performance Report
// Compile: clang++ -std=c++20 -O2 -framework CoreServices MacEverything/Core/*.cpp test_all.cpp -o test_all
// Run:     ./test_all [root_path]            (all tests, default root: /)
//          ./test_all --fast                 (fast unit tests only: 3, 3b, 3c, 3d, 3e, 5)
//          ./test_all --slow [root_path]     (slow integration tests: 1, 4, 6)
//          ./test_all --part 3 --part 3b     (specific parts)

#include "MacEverything/Core/DirectoryScanner.h"
#include "MacEverything/Core/SearchEngine.h"
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
