#include "IndexPersistence.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cmath>

IndexPersistence::IndexPersistence(std::shared_ptr<SearchEngine> engine,
                                   const std::string& basePath,
                                   const std::string& walPath,
                                   const std::string& pagesPath,
                                   const std::string& ptablePath)
    : engine_(std::move(engine))
    , basePath_(basePath)
    , walPath_(walPath)
    , pagedWriter_(std::make_unique<PagedIndexWriter>(pagesPath, ptablePath))
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

    // 1. Try paged format first (v4)
    bool loaded = false;
    if (pagedWriter_->exists()) {
        IndexMetadata meta;
        loaded = pagedWriter_->load(*engine_, &meta);
        if (loaded) {
            lastEventId = meta.lastEventId;
            LOG_INFO("IndexPersistence", "Loaded paged index, lastEventId=" << lastEventId
                      << ", records=" << engine_->liveRecordCount());
        } else {
            LOG_ERROR("IndexPersistence", "Paged index corrupt, trying legacy format");
        }
    }

    // 2. Fallback to legacy v3 format
    if (!loaded) {
        loaded = engine_->loadFromFile(basePath_, &lastEventId);
        if (loaded) {
            LOG_INFO("IndexPersistence", "Loaded legacy index, lastEventId=" << lastEventId
                      << ", records=" << engine_->liveRecordCount());
            // Migrate: write out paged format for next time
            IndexMetadata migrateMeta;
            migrateMeta.lastEventId = lastEventId;
            if (pagedWriter_->fullRewrite(*engine_, migrateMeta)) {
                LOG_INFO("IndexPersistence", "Migrated legacy index to paged format");
            }
        } else {
            LOG_INFO("IndexPersistence", "No base index found at " << basePath_);
        }
    }

    // 3. Batch-merge WAL entries into base records
    auto entries = IndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        LOG_INFO("IndexPersistence", "Replaying " << entries.size() << " WAL entries (batch mode)");

        auto records = engine_->exportRecords();

        std::unordered_map<std::string, size_t> pathMap;
        pathMap.reserve(records.size());
        for (size_t i = 0; i < records.size(); i++) {
            std::string fullPath = SearchEngine::makeFullPath(records[i].path, records[i].name);
            pathMap[me::toLower(fullPath)] = i;
        }

        for (auto& entry : entries) {
            std::string lowerPath = me::toLower(entry.fullPath);

            switch (entry.op) {
                case WALOp::Add: {
                    auto it = pathMap.find(lowerPath);
                    if (it != pathMap.end()) {
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
                        records[it->second].type = 0;
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

        records.erase(
            std::remove_if(records.begin(), records.end(),
                [](const FileRecord& r) { return r.type == 0; }),
            records.end());

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

void IndexPersistence::flush(uint64_t lastEventId, bool force) {
    IndexMetadata meta;
    meta.lastEventId = lastEventId;
    flush(meta, force);
}

void IndexPersistence::flush(const IndexMetadata& metadata, bool force) {
    // Skip if no mutations
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_ && !wal_->isDirty()) {
            LOG_INFO("IndexPersistence", "Skipping flush — no mutations since last flush");
            return;
        }
        if (!force && wal_ && wal_->entryCount() < kCompactThreshold) {
            LOG_INFO("IndexPersistence", "Skipping flush — only "
                      << wal_->entryCount() << " entries (threshold=" << kCompactThreshold << ")");
            return;
        }
    }

    // Check if full compaction is needed (tombstone ratio)
    uint32_t totalCount = engine_->recordCount();
    uint32_t liveCount = engine_->liveRecordCount();
    double tombstoneRatio = totalCount > 0
        ? static_cast<double>(totalCount - liveCount) / totalCount
        : 0.0;

    if (tombstoneRatio > kTombstoneCompactRatio) {
        LOG_INFO("IndexPersistence", "Tombstone ratio " << (tombstoneRatio * 100)
                  << "% > " << (kTombstoneCompactRatio * 100) << "% — triggering full compaction");
        fullCompact(metadata);
        return;
    }

    // Check if dead space rewrite is needed
    if (pagedWriter_->deadSpaceRatio() > kDeadSpaceRewriteRatio) {
        LOG_INFO("IndexPersistence", "Dead space ratio " << (pagedWriter_->deadSpaceRatio() * 100)
                  << "% — triggering full rewrite to reclaim space");
    }

    // 1. Open a fresh WAL before detaching the old one
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("IndexPersistence", "Failed to open new WAL for flush");
        return;
    }

    // 2. Atomically swap WAL
    std::shared_ptr<IndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }
    engine_->attachWAL(newWal);

    // 3. Flush dirty pages (or full rewrite if dead space is high)
    bool writeOk;
    if (engine_->needsFullRewrite() || pagedWriter_->deadSpaceRatio() > kDeadSpaceRewriteRatio) {
        writeOk = pagedWriter_->fullRewrite(*engine_, metadata);
    } else {
        writeOk = pagedWriter_->flushDirtyPages(*engine_, metadata);
    }

    if (writeOk) {
        LOG_INFO("IndexPersistence", "Flushed paged index, lastEventId=" << metadata.lastEventId
                  << ", records=" << engine_->liveRecordCount());
    } else {
        LOG_ERROR("IndexPersistence", "Failed to flush paged index — keeping old WAL for recovery");
        if (oldWal) oldWal->close();
        return;
    }

    // 4. Rename new WAL to replace old
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("IndexPersistence", "Failed to rename WAL: " << newWalPath
                  << " -> " << walPath_ << " (errno=" << errno << ": " << strerror(errno) << ")");
    } else {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->updatePath(walPath_);
    }

    // 5. Close old WAL
    if (oldWal) oldWal->close();
}

void IndexPersistence::fullCompact(const IndexMetadata& metadata) {
    // 1. Open a fresh WAL
    auto newWal = std::make_shared<IndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("IndexPersistence", "Failed to open new WAL for full compaction");
        return;
    }

    // 2. Swap WAL
    std::shared_ptr<IndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }
    engine_->attachWAL(newWal);

    // 3. Compact in-memory records (remove tombstones)
    auto remap = engine_->compactRecords();
    if (!remap.empty() && contentIndex_) {
        contentIndex_->remapFileIndices(remap);
    }

    // 4. Full rewrite paged files
    if (pagedWriter_->fullRewrite(*engine_, metadata)) {
        LOG_INFO("IndexPersistence", "Full compaction done, lastEventId=" << metadata.lastEventId
                  << ", records=" << engine_->liveRecordCount());
    } else {
        LOG_ERROR("IndexPersistence", "Failed to write full compaction — keeping old WAL");
        if (oldWal) oldWal->close();
        return;
    }

    // 5. Rename WAL
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("IndexPersistence", "Failed to rename WAL: " << newWalPath
                  << " -> " << walPath_);
    } else {
        std::lock_guard<std::mutex> lock(walMutex_);
        if (wal_) wal_->updatePath(walPath_);
    }

    // 6. Close old WAL
    if (oldWal) oldWal->close();
}

double IndexPersistence::computeAdaptiveInterval() const {
    auto dirtyPages = engine_->getDirtyPageNumbers();
    uint32_t dirtyCount = static_cast<uint32_t>(dirtyPages.size());
    uint32_t liveCount = engine_->liveRecordCount();
    uint32_t totalPages = (liveCount + SearchEngine::kRecordsPerPage - 1) / SearchEngine::kRecordsPerPage;
    if (totalPages == 0) totalPages = 1;

    double dirtyRatio = static_cast<double>(dirtyCount) / totalPages;

    double interval;
    if (dirtyRatio > 0.3) {
        interval = kMinIntervalSec;
    } else if (dirtyRatio > 0.1) {
        interval = kBaseIntervalSec * 0.5;
    } else if (dirtyRatio > 0.01) {
        interval = kBaseIntervalSec;
    } else {
        interval = kMaxIntervalSec;
    }

    // WAL size override
    size_t walSize = 0;
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(walMutex_));
        if (wal_) walSize = wal_->currentSize();
    }
    if (walSize > kWALSizeFlushThreshold) {
        interval = std::min(interval, kMinIntervalSec);
    }

    return interval;
}

void IndexPersistence::rescheduleTimer(double intervalSec) {
    if (!compactionTimer_) return;
    if (std::abs(intervalSec - currentIntervalSec_) < 1.0) return; // no significant change

    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(compactionTimer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);
    currentIntervalSec_ = intervalSec;
    LOG_INFO("IndexPersistence", "Adaptive interval adjusted to " << intervalSec << "s");
}

void IndexPersistence::startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher) {
    stopAutoCompactionAndWait();

    currentIntervalSec_ = intervalSec;
    compactionQueue_ = dispatch_queue_create("com.maceverything.index.compaction", DISPATCH_QUEUE_SERIAL);
    compactionTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, compactionQueue_);
    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(compactionTimer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);

    auto* self = this;
    dispatch_source_set_event_handler(compactionTimer_, ^{
        uint64_t eventId = watcher ? watcher->getLastEventId() : 0;
        self->flush(eventId);

        // Adjust interval based on current state
        double newInterval = self->computeAdaptiveInterval();
        self->rescheduleTimer(newInterval);
    });

    dispatch_resume(compactionTimer_);
    LOG_INFO("IndexPersistence", "Auto-compaction started (initial interval " << intervalSec << "s)");
}

void IndexPersistence::stopAutoCompactionAndWait() {
    if (compactionTimer_) {
        dispatch_source_cancel(compactionTimer_);
        dispatch_release(compactionTimer_);
        compactionTimer_ = nullptr;
    }
    if (compactionQueue_) {
        dispatch_sync(compactionQueue_, ^{});
        dispatch_release(compactionQueue_);
        compactionQueue_ = nullptr;
    }
}
