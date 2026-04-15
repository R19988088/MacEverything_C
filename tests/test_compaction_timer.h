#pragma once
// Part 33: CompactionTimer tests

static void runCompactionTimerTests() {
    std::cout << "═══ Part 33: CompactionTimer Tests ═══\n\n";

    // Test 1: start and callback fires
    {
        std::atomic<int> fireCount{0};
        std::mutex mtx;
        std::condition_variable cv;

        CompactionTimer timer;
        timer.start(0.1, [&]{
            fireCount.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mtx);
            cv.notify_one();
        }, "com.maceverything.test.timer1");

        check(timer.isRunning(), "CompactionTimer: isRunning after start");

        // Wait for at least one fire
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(2), [&]{ return fireCount.load() > 0; });
        }
        check(fireCount.load() > 0, "CompactionTimer: callback fired at least once");

        timer.stopAndWait();
        check(!timer.isRunning(), "CompactionTimer: not running after stop");
    }

    // Test 2: reschedule changes interval
    {
        CompactionTimer timer;
        timer.start(10.0, []{}, "com.maceverything.test.timer2");
        check(std::abs(timer.currentInterval() - 10.0) < 0.01, "CompactionTimer: initial interval 10s");

        timer.reschedule(2.0);
        check(std::abs(timer.currentInterval() - 2.0) < 0.01, "CompactionTimer: reschedule to 2s");

        // Small change should be ignored
        timer.reschedule(2.5);
        check(std::abs(timer.currentInterval() - 2.0) < 0.01, "CompactionTimer: reschedule ignored for <1s change");

        timer.stopAndWait();
    }

    // Test 3: double start replaces timer
    {
        std::atomic<int> count1{0}, count2{0};

        CompactionTimer timer;
        timer.start(0.05, [&]{ count1++; }, "com.maceverything.test.timer3a");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        int c1 = count1.load();

        timer.start(0.05, [&]{ count2++; }, "com.maceverything.test.timer3b");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        int c1After = count1.load();
        int c2 = count2.load();

        check(c1After == c1, "CompactionTimer: old callback stopped after restart");
        check(c2 > 0, "CompactionTimer: new callback fires after restart");

        timer.stopAndWait();
    }

    // Test 4: destructor stops cleanly
    {
        std::atomic<bool> fired{false};
        {
            CompactionTimer timer;
            timer.start(0.05, [&]{ fired.store(true); }, "com.maceverything.test.timer4");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Should not crash
        check(true, "CompactionTimer: destructor completes without crash");
    }

    std::cout << "\n";
}
