#include "CompactionTimer.h"
#include "Logger.h"
#include <cmath>

CompactionTimer::~CompactionTimer() {
    stopAndWait();
}

void CompactionTimer::start(double intervalSec, std::function<void()> callback, const char* queueLabel) {
    stopAndWait();

    currentIntervalSec_ = intervalSec;
    queue_ = dispatch_queue_create(queueLabel, DISPATCH_QUEUE_SERIAL);
    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);

    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(timer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);

    // Move callback into a shared_ptr so the block can capture it safely
    auto cb = std::make_shared<std::function<void()>>(std::move(callback));
    dispatch_source_set_event_handler(timer_, ^{
        (*cb)();
    });

    dispatch_resume(timer_);
}

void CompactionTimer::stopAndWait() {
    if (timer_) {
        dispatch_source_cancel(timer_);
        dispatch_release(timer_);
        timer_ = nullptr;
    }
    if (queue_) {
        dispatch_sync(queue_, ^{});
        dispatch_release(queue_);
        queue_ = nullptr;
    }
    currentIntervalSec_ = 0;
}

void CompactionTimer::reschedule(double intervalSec) {
    if (!timer_) return;
    if (std::abs(intervalSec - currentIntervalSec_) < 1.0) return;

    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(timer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);
    currentIntervalSec_ = intervalSec;
}
