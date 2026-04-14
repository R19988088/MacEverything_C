#pragma once
// Part 17: Query Cancellation Tests
// Validates that a new query() call cancels in-flight dispatch_apply scans
// via the queryGeneration_ atomic counter.

inline void runQueryCancelTests() {
    std::cout << "=== Part 17: Query Cancellation ===\n\n";

    // Build a large engine (100k records) so query() takes measurable time
    constexpr uint32_t N = 100'000;
    SearchEngine engine;
    {
        std::vector<FileRecord> records;
        records.reserve(N);
        for (uint32_t i = 0; i < N; i++) {
            FileRecord r;
            r.name = "file_match_" + std::to_string(i) + ".txt";
            r.path = "/deep/nested/path/number/" + std::to_string(i % 1000);
            r.type = 1;
            r.size = 100;
            r.modTime = static_cast<time_t>(i);
            records.push_back(std::move(r));
        }
        engine.loadRecords(std::move(records));
    }

    // Test 1: Concurrent queries — second should cancel first
    {
        std::atomic<bool> firstDone{false};
        std::atomic<double> firstMs{0};
        std::atomic<size_t> firstResultCount{0};

        // Launch first query on background thread (searches path, triggers full linear scan)
        std::thread t1([&] {
            auto start = std::chrono::steady_clock::now();
            auto results = engine.query("deep/nested", 10000);
            auto elapsed = std::chrono::steady_clock::now() - start;
            firstMs.store(std::chrono::duration<double, std::milli>(elapsed).count(),
                          std::memory_order_relaxed);
            firstResultCount.store(results.size(), std::memory_order_relaxed);
            firstDone.store(true, std::memory_order_release);
        });

        // Small delay to let t1 enter dispatch_apply
        std::this_thread::sleep_for(std::chrono::microseconds(500));

        // Second query — should bump generation, causing first to abort
        auto start2 = std::chrono::steady_clock::now();
        auto results2 = engine.query("file_match_42", 100);
        auto elapsed2 = std::chrono::steady_clock::now() - start2;
        double secondMs = std::chrono::duration<double, std::milli>(elapsed2).count();

        t1.join();

        // The first query should have returned early with few/no results (cancelled)
        // The second query should have completed successfully with results
        bool firstCancelled = firstResultCount.load(std::memory_order_relaxed) == 0;
        bool secondHasResults = !results2.empty();

        std::cout << "    First query: " << firstMs.load() << " ms, "
                  << firstResultCount.load() << " results"
                  << (firstCancelled ? " (cancelled)" : " (completed)") << "\n";
        std::cout << "    Second query: " << secondMs << " ms, "
                  << results2.size() << " results\n";

        // The key invariant: second query returns results (it wasn't starved)
        check(secondHasResults, "Second query returns results (not starved by first)");

        // The first query should have been cancelled (returned empty)
        check(firstCancelled, "First query was cancelled (returned empty results)");
    }

    // Test 2: Empty-string query bumps generation (cancels in-flight)
    {
        std::atomic<size_t> resultCount{0};

        std::thread t1([&] {
            auto results = engine.query("deep/nested", 10000);
            resultCount.store(results.size(), std::memory_order_relaxed);
        });

        std::this_thread::sleep_for(std::chrono::microseconds(500));

        // Empty query should bump generation even though it returns immediately
        auto emptyResult = engine.query("", 0);
        check(emptyResult.empty(), "Empty query returns empty results");

        t1.join();
        bool cancelled = resultCount.load(std::memory_order_relaxed) == 0;
        check(cancelled, "In-flight query cancelled by empty query");
    }

    // Test 3: Single query without interference completes normally
    {
        auto results = engine.query("file_match_99", 100);
        check(!results.empty(), "Normal query without interference returns results");
    }
}
