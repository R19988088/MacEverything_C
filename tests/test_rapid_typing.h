#pragma once
// Part 19: Rapid Typing Stress Test
// Simulates user rapidly typing/deleting/retyping to measure query latency
// and cancellation behavior under high-frequency query load.

inline void runRapidTypingTest() {
    std::cout << "=== Part 19: Rapid Typing Stress Test ===\n\n";

    // Build a large engine (100k records) so query() takes measurable time
    constexpr uint32_t N = 100'000;
    SearchEngine engine;
    {
        std::vector<FileRecord> records;
        records.reserve(N);
        for (uint32_t i = 0; i < N; i++) {
            FileRecord r;
            // Spread names to avoid trivial trigram hits for short prefixes
            r.name = "hello_world_file_" + std::to_string(i) + ".txt";
            r.path = "/deep/nested/path/number/" + std::to_string(i % 1000);
            r.type = 1;
            r.size = 100;
            r.modTime = static_cast<time_t>(i);
            records.push_back(std::move(r));
        }
        engine.loadRecords(std::move(records));
    }

    // ── Scenario 1: Incremental typing "h" → "he" → "hel" → "hell" → "hello" ──
    {
        std::cout << "  Scenario 1: Incremental typing simulation\n";

        const std::vector<std::string> keystrokes = {"h", "he", "hel", "hell", "hello"};

        // Fire queries rapidly from background threads, simulating debounce arrivals
        std::vector<std::thread> threads;
        std::vector<std::atomic<size_t>> resultCounts(keystrokes.size());
        for (auto& rc : resultCounts) rc.store(0, std::memory_order_relaxed);

        // Launch intermediate queries (will be cancelled by subsequent ones)
        for (size_t i = 0; i < keystrokes.size() - 1; i++) {
            threads.emplace_back([&engine, &resultCounts, &keystrokes, i] {
                auto results = engine.query(keystrokes[i], 10000);
                resultCounts[i].store(results.size(), std::memory_order_relaxed);
            });
            // Small delay between "keystrokes" (30ms simulates post-debounce interval)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        // Final query — this one should complete with results
        auto finalStart = std::chrono::steady_clock::now();
        auto finalResults = engine.query(keystrokes.back(), 10000);
        auto finalElapsed = std::chrono::steady_clock::now() - finalStart;
        double finalMs = std::chrono::duration<double, std::milli>(finalElapsed).count();

        for (auto& t : threads) t.join();

        // Count how many intermediate queries were cancelled
        size_t cancelledCount = 0;
        for (size_t i = 0; i < keystrokes.size() - 1; i++) {
            if (resultCounts[i].load(std::memory_order_relaxed) == 0) {
                cancelledCount++;
            }
        }

        std::cout << "    Intermediate queries cancelled: " << cancelledCount
                  << "/" << (keystrokes.size() - 1) << "\n";
        std::cout << "    Final query (\"hello\"): " << finalMs << " ms, "
                  << finalResults.size() << " results\n";

        check(!finalResults.empty(),
              "Scenario 1: Final query returns results");
        check(finalMs < 200.0,
              "Scenario 1: Final query latency < 200ms");
    }

    // ── Scenario 2: Type → Clear → Retype ──
    {
        std::cout << "\n  Scenario 2: Type → Clear → Retype\n";

        // Phase 1: Start a slow query
        std::atomic<size_t> phase1Results{0};
        std::thread t1([&] {
            auto results = engine.query("deep/nested", 10000);
            phase1Results.store(results.size(), std::memory_order_relaxed);
        });

        std::this_thread::sleep_for(std::chrono::microseconds(500));

        // Phase 2: User clears the search box (empty string cancels in-flight)
        auto emptyResults = engine.query("", 0);

        // Phase 3: Small pause then retype (simulates user thinking + new input)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto retypeStart = std::chrono::steady_clock::now();
        auto retypeResults = engine.query("hello_world_file_42", 100);
        auto retypeElapsed = std::chrono::steady_clock::now() - retypeStart;
        double retypeMs = std::chrono::duration<double, std::milli>(retypeElapsed).count();

        t1.join();

        bool phase1Cancelled = phase1Results.load(std::memory_order_relaxed) == 0;

        std::cout << "    Phase 1 (\"deep/nested\"): "
                  << phase1Results.load() << " results"
                  << (phase1Cancelled ? " (cancelled)" : " (completed)") << "\n";
        std::cout << "    Phase 2 (clear): " << emptyResults.size() << " results\n";
        std::cout << "    Phase 3 (\"hello_world_file_42\"): " << retypeMs << " ms, "
                  << retypeResults.size() << " results\n";

        check(emptyResults.empty(),
              "Scenario 2: Empty query returns empty");
        check(!retypeResults.empty(),
              "Scenario 2: Retype query returns correct results");
        check(retypeMs < 200.0,
              "Scenario 2: Retype latency < 200ms");
    }

    // ── Scenario 3: High-frequency stress (100 rapid queries) ──
    {
        std::cout << "\n  Scenario 3: High-frequency stress (100 rapid queries)\n";

        constexpr int QUERY_COUNT = 100;
        std::vector<std::thread> threads;
        std::vector<std::atomic<size_t>> resultCounts(QUERY_COUNT);
        for (auto& rc : resultCounts) rc.store(0, std::memory_order_relaxed);

        auto stressStart = std::chrono::steady_clock::now();

        // Fire 99 "intermediate" queries rapidly from background threads
        for (int i = 0; i < QUERY_COUNT - 1; i++) {
            threads.emplace_back([&engine, &resultCounts, i] {
                std::string keyword = "file_stress_query_" + std::to_string(i);
                auto results = engine.query(keyword, 1000);
                resultCounts[i].store(results.size(), std::memory_order_relaxed);
            });
            // 10ms between queries — faster than real typing, stress test
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Final query — should complete correctly
        auto finalStart = std::chrono::steady_clock::now();
        auto finalResults = engine.query("hello_world_file_99", 100);
        auto finalElapsed = std::chrono::steady_clock::now() - finalStart;
        double finalMs = std::chrono::duration<double, std::milli>(finalElapsed).count();

        for (auto& t : threads) t.join();

        auto stressElapsed = std::chrono::steady_clock::now() - stressStart;
        double totalMs = std::chrono::duration<double, std::milli>(stressElapsed).count();

        // Count cancellations
        size_t cancelledCount = 0;
        for (int i = 0; i < QUERY_COUNT - 1; i++) {
            if (resultCounts[i].load(std::memory_order_relaxed) == 0) {
                cancelledCount++;
            }
        }

        double cancelRate = 100.0 * cancelledCount / (QUERY_COUNT - 1);

        std::cout << "    Total wall time: " << totalMs << " ms\n";
        std::cout << "    Cancelled: " << cancelledCount << "/" << (QUERY_COUNT - 1)
                  << " (" << std::fixed << std::setprecision(1) << cancelRate << "%)\n";
        std::cout << "    Final query: " << finalMs << " ms, "
                  << finalResults.size() << " results\n";

        check(!finalResults.empty(),
              "Scenario 3: Final query returns results under stress");
        check(finalMs < 200.0,
              "Scenario 3: Final query latency < 200ms under stress");
        // At least some intermediate queries should be cancelled
        check(cancelledCount > 0,
              "Scenario 3: Some intermediate queries were cancelled");
    }

    // ── Scenario 4: Baseline latency (no contention) ──
    {
        std::cout << "\n  Scenario 4: Baseline latency (no contention)\n";

        // Warm up
        engine.query("hello_world_file_0", 10);

        // Measure baseline for a trigram-accelerated query
        constexpr int RUNS = 10;
        double totalMs = 0;
        size_t totalResults = 0;
        for (int i = 0; i < RUNS; i++) {
            auto start = std::chrono::steady_clock::now();
            auto results = engine.query("hello_world_file_" + std::to_string(i * 100), 100);
            auto elapsed = std::chrono::steady_clock::now() - start;
            totalMs += std::chrono::duration<double, std::milli>(elapsed).count();
            totalResults += results.size();
        }
        double avgMs = totalMs / RUNS;

        // Measure baseline for a path scan (slow path)
        double pathTotalMs = 0;
        for (int i = 0; i < RUNS; i++) {
            auto start = std::chrono::steady_clock::now();
            auto results = engine.query("deep/nested", 100);
            auto elapsed = std::chrono::steady_clock::now() - start;
            pathTotalMs += std::chrono::duration<double, std::milli>(elapsed).count();
        }
        double pathAvgMs = pathTotalMs / RUNS;

        std::cout << "    Trigram query avg: " << std::fixed << std::setprecision(2)
                  << avgMs << " ms (" << totalResults / RUNS << " results avg)\n";
        std::cout << "    Path scan avg:    " << std::fixed << std::setprecision(2)
                  << pathAvgMs << " ms\n";

        check(avgMs < 50.0,
              "Scenario 4: Trigram query baseline < 50ms");
        check(pathAvgMs < 200.0,
              "Scenario 4: Path scan baseline < 200ms");
    }

    std::cout << "\n";
}
