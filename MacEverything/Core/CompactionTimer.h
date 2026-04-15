#pragma once
#include <functional>
#include <dispatch/dispatch.h>

/// Shared GCD timer for periodic compaction/flush operations.
/// Wraps dispatch_source_t + serial queue boilerplate used by both
/// IndexPersistence and ContentIndexPersistence.
class CompactionTimer {
public:
    CompactionTimer() = default;
    ~CompactionTimer();

    CompactionTimer(const CompactionTimer&) = delete;
    CompactionTimer& operator=(const CompactionTimer&) = delete;

    /// Start the timer. Replaces any previous timer.
    /// @param intervalSec  Initial fire interval in seconds.
    /// @param callback     Block invoked on each fire (on a serial queue).
    /// @param queueLabel   Label for the GCD serial queue (e.g. "com.maceverything.index.compaction").
    void start(double intervalSec, std::function<void()> callback, const char* queueLabel);

    /// Stop the timer and synchronously wait for any in-flight callback to finish.
    void stopAndWait();

    /// Change the fire interval. No-op if the change is < 1 second.
    void reschedule(double intervalSec);

    bool isRunning() const { return timer_ != nullptr; }
    double currentInterval() const { return currentIntervalSec_; }

private:
    dispatch_source_t timer_ = nullptr;
    dispatch_queue_t  queue_ = nullptr;
    double currentIntervalSec_ = 0;
};
