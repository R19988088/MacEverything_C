#include "IndexPersistence.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>

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
        LOG_INFO("IndexPersistence", "Loaded base index, lastEventId=" << lastEventId
                  << ", records=" << engine_->liveRecordCount());
    } else {
        LOG_INFO("IndexPersistence", "No base index found at " << basePath_);
    }

    // 2. Batch-merge WAL entries into base records, then load once.
    //    This avoids per-entry lock+trigram-update overhead that caused timeouts
    //    with large WALs (190K+ entries).
    auto entries = IndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        LOG_INFO("IndexPersistence", "Replaying " << entries.size() << " WAL entries (batch mode)");

        // Export current records with paths restored
        auto records = engine_->exportRecords();

        // Build path->index map for efficient lookup (case-insensitive)
        std::unordered_map<std::string, size_t> pathMap;
        pathMap.reserve(records.size());
        for (size_t i = 0; i < records.size(); i++) {
            std::string fullPath = SearchEngine::makeFullPath(records[i].path, records[i].name);
            pathMap[me::toLower(fullPath)] = i;
        }

        // Apply WAL entries to the records vector
        for (auto& entry : entries) {
            std::string lowerPath = me::toLower(entry.fullPath);

            switch (entry.op) {
                case WALOp::Add: {
                    auto it = pathMap.find(lowerPath);
                    if (it != pathMap.end()) {
                        // Path already exists — update in place (idempotency)
                        records[it->second] = std::move(entry.record);
                    } else {
                        pathMap[lowerPath] = records.size();
                        records.push_back(std::move(entry.record));
                    }
                    break;
                }
                case WALOp::Remove: {
                    auto it = pathMap.find(lowerPath);
                    if (it != pathMap.end()) {
                        records[it->second].type = 0; // tombstone
                        pathMap.erase(it);
                    }
                    break;
                }
                case WALOp::Update: {
                    auto it = pathMap.find(lowerPath);
                    if (it != pathMap.end()) {
                        records[it->second] = std::move(entry.record);
                    } else {
                        pathMap[lowerPath] = records.size();
                        records.push_back(std::move(entry.record));
                    }
                    break;
                }
            }
        }

        // Remove tombstones before batch loading
        records.erase(
            std::remove_if(records.begin(), records.end(),
                [](const FileRecord& r) { return r.type == 0; }),
            records.end());

        // Batch load all records at once (efficient trigram build + path interning)
        engine_->loadRecords(std::move(records));
        LOG_INFO("IndexPersistence", "WAL replay done, live records=" << engine_->liveRecordCount());
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
        LOG_INFO("IndexPersistence", "WAL attached at " << walPath_);
    } else {
        LOG_ERROR("IndexPersistence", "Failed to open WAL at " << walPath_);
    }
}

void IndexPersistence::compact(uint64_t lastEventId, bool force) {
    IndexMetadata meta;
    meta.lastEventId = lastEventId;
    compact(meta, force);
}

void IndexPersistence::compact(const IndexMetadata& metadata, bool force) {
    // Skip compaction if no mutations since last compact
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_ && !wal_->isDirty()) {
            LOG_INFO("IndexPersistence", "Skipping compaction — no mutations since last compact");
            return;
        }
        // Skip compaction if WAL has too few entries (not worth the I/O cost)
        if (!force && wal_ && wal_->entryCount() < kCompactThreshold) {
            LOG_INFO("IndexPersistence", "Skipping compaction — only "
                      << wal_->entryCount() << " entries (threshold=" << kCompactThreshold << ")");
            return;
        }
    }

    // 1. Open a fresh WAL *before* detaching the old one, so there is no
    //    window where mutations are unlogged.
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("IndexPersistence", "Failed to open new WAL for compaction");
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

    // 4. Write base file (crash-safety: C-2 fix — write before removing old WAL)
    if (engine_->saveToFile(basePath_, metadata)) {
        LOG_INFO("IndexPersistence", "Compacted base index, lastEventId=" << metadata.lastEventId
                  << ", records=" << engine_->liveRecordCount());
    } else {
        LOG_ERROR("IndexPersistence", "Failed to write base index — keeping old WAL for recovery");
        if (oldWal) {
            oldWal->close();
        }
        return;
    }

    // 5. Rename new WAL from .wal.new to .wal.
    //    POSIX rename atomically replaces the old .wal directory entry,
    //    so old WAL's inode is unlinked from the directory — its fd remains
    //    valid but the file is gone once the fd is closed.
    //    This avoids the self-propagating failure chain: we never call
    //    closeAndDelete() on oldWal, so there's no risk of unlinking the
    //    new WAL's file.
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("IndexPersistence", "Failed to rename WAL: " << newWalPath
                  << " -> " << walPath_ << " (errno=" << errno << ": " << strerror(errno) << ")");
    } else {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->updatePath(walPath_);
    }

    // 6. Close old WAL (just close fd — rename already replaced the directory entry)
    if (oldWal) {
        oldWal->close();
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
    LOG_INFO("IndexPersistence", "Auto-compaction started (every " << intervalSec << "s)");
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

