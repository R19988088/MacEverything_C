#include "IndexPersistence.h"
#include <iostream>
#include <chrono>

IndexPersistence::IndexPersistence(std::shared_ptr<SearchEngine> engine,
                                   const std::string& basePath,
                                   const std::string& walPath)
    : engine_(std::move(engine))
    , basePath_(basePath)
    , walPath_(walPath)
{}

IndexPersistence::~IndexPersistence() {
    stopAutoCompactionAndWait();
    if (engine_) engine_->detachWAL();
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->close();
    }
}

uint64_t IndexPersistence::load() {
    uint64_t lastEventId = 0;

    // 1. Load base index
    bool loaded = engine_->loadFromFile(basePath_, &lastEventId);
    if (loaded) {
        std::cout << "[IndexPersistence] Loaded base index, lastEventId=" << lastEventId
                  << ", records=" << engine_->liveRecordCount() << "\n";
    } else {
        std::cout << "[IndexPersistence] No base index found at " << basePath_ << "\n";
    }

    // 2. Replay WAL entries on top (with 15s timeout)
    auto entries = IndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        std::cout << "[IndexPersistence] Replaying " << entries.size() << " WAL entries\n";
        auto replayStart = std::chrono::steady_clock::now();
        bool timedOut = false;
        for (auto& entry : entries) {
            if (std::chrono::steady_clock::now() - replayStart > std::chrono::seconds(15)) {
                std::cout << "[IndexPersistence] WAL replay timeout (15s) — forcing full scan\n";
                timedOut = true;
                break;
            }
            switch (entry.op) {
                case WALOp::Add:
                    engine_->addRecord(std::move(entry.record));
                    break;
                case WALOp::Remove:
                    engine_->removeByPath(entry.fullPath);
                    break;
                case WALOp::Update:
                    engine_->updateByPath(entry.fullPath, std::move(entry.record));
                    break;
            }
        }
        if (timedOut) {
            // Reset engine and return 0 to force full scan
            engine_->loadRecords({});
            return 0;
        }
        std::cout << "[IndexPersistence] WAL replay done, live records=" << engine_->liveRecordCount() << "\n";
    }

    return lastEventId;
}

void IndexPersistence::attachWAL() {
    auto newWal = std::make_shared<IndexWAL>();
    if (newWal->open(walPath_)) {
        {
            std::lock_guard<std::mutex> lock(walMutex_);
            wal_ = newWal;
        }
        engine_->attachWAL(newWal);
        std::cout << "[IndexPersistence] WAL attached at " << walPath_ << "\n";
    } else {
        std::cerr << "[IndexPersistence] Failed to open WAL at " << walPath_ << "\n";
    }
}

void IndexPersistence::compact(uint64_t lastEventId) {
    IndexMetadata meta;
    meta.lastEventId = lastEventId;
    compact(meta);
}

void IndexPersistence::compact(const IndexMetadata& metadata) {
    // 1. Open a fresh WAL *before* detaching the old one, so there is no
    //    window where mutations are unlogged.
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        std::cerr << "[IndexPersistence] Failed to open new WAL for compaction\n";
        return;
    }

    // 2. Atomically swap: attach new WAL, get old WAL back
    std::shared_ptr<IndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }
    engine_->attachWAL(newWal);

    // 3. Compact in-memory records (remove tombstones) before saving
    auto remap = engine_->compactRecords();
    // H-1: Propagate index remap to ContentIndex
    if (!remap.empty() && contentIndex_) {
        contentIndex_->remapFileIndices(remap);
    }

    // 4. Write base file BEFORE deleting old WAL (C-2 fix: crash-safe ordering).
    //    If saveToFile fails, old WAL is preserved so no data is lost.
    if (engine_->saveToFile(basePath_, metadata)) {
        std::cout << "[IndexPersistence] Compacted base index, lastEventId=" << metadata.lastEventId
                  << ", records=" << engine_->liveRecordCount() << "\n";
        // 5. Only delete old WAL after base file is successfully written
        if (oldWal) {
            oldWal->closeAndDelete();
        }
    } else {
        std::cerr << "[IndexPersistence] Failed to write base index — keeping old WAL for recovery\n";
        // Old WAL is preserved; on next startup, its entries will be replayed
        if (oldWal) {
            oldWal->close();
        }
    }

    // 6. Rename new WAL to standard path
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        std::cerr << "[IndexPersistence] Failed to rename WAL: " << newWalPath << " -> " << walPath_ << "\n";
    }
}

void IndexPersistence::startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher) {
    stopAutoCompactionAndWait();

    // Use a dedicated serial queue so we can dispatch_sync to drain in-flight work
    compactionQueue_ = dispatch_queue_create("com.maceverything.index.compaction", DISPATCH_QUEUE_SERIAL);
    compactionTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, compactionQueue_);
    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(compactionTimer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC); // 30s leeway

    // H9 fix: Capture shared_ptr — the block's copy keeps the watcher alive
    // as long as the timer exists, preventing use-after-free.
    auto* self = this;
    dispatch_source_set_event_handler(compactionTimer_, ^{
        uint64_t eventId = watcher ? watcher->getLastEventId() : 0;
        self->compact(eventId);
    });

    dispatch_resume(compactionTimer_);
    std::cout << "[IndexPersistence] Auto-compaction started (every " << intervalSec << "s)\n";
}

void IndexPersistence::stopAutoCompactionAndWait() {
    if (compactionTimer_) {
        dispatch_source_cancel(compactionTimer_);
        dispatch_release(compactionTimer_);
        compactionTimer_ = nullptr;
    }
    if (compactionQueue_) {
        // Drain in-flight compaction by synchronously dispatching a no-op
        dispatch_sync(compactionQueue_, ^{});
        dispatch_release(compactionQueue_);
        compactionQueue_ = nullptr;
    }
}

