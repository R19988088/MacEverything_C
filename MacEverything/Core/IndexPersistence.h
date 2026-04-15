#pragma once
#include "SearchEngine.h"
#include "ContentIndex.h"
#include "IndexWAL.h"
#include "PagedIndexWriter.h"
#include "FileSystemWatcher.h"
#include <string>
#include <memory>
#include <mutex>
#include <dispatch/dispatch.h>

/// Orchestrates index persistence: paged base files + WAL + auto-compaction.
class IndexPersistence {
public:
    IndexPersistence(std::shared_ptr<SearchEngine> engine,
                     const std::string& basePath,
                     const std::string& walPath,
                     const std::string& pagesPath,
                     const std::string& ptablePath);
    ~IndexPersistence();

    IndexPersistence(const IndexPersistence&) = delete;
    IndexPersistence& operator=(const IndexPersistence&) = delete;

    /// Load base index + replay WAL entries.
    /// Tries paged format first, falls back to v3 format, auto-migrates.
    /// Returns the lastEventId from the base file (0 if no file or v1 format).
    uint64_t load();

    /// Start logging mutations to WAL.
    void attachWAL();

    /// Minimum WAL entry count before flush proceeds (unless forced).
    static constexpr uint64_t kCompactThreshold = 100;

    /// Incremental flush: write only dirty pages, swap WAL.
    void flush(uint64_t lastEventId, bool force = false);
    void flush(const IndexMetadata& metadata, bool force = false);

    /// Full compaction: compactRecords() + fullRewrite() + remap ContentIndex.
    /// Called when tombstone ratio > kTombstoneCompactRatio.
    void fullCompact(const IndexMetadata& metadata);

    /// Legacy compact() — delegates to flush() for backward compatibility.
    void compact(uint64_t lastEventId, bool force = false) { flush(lastEventId, force); }
    void compact(const IndexMetadata& metadata, bool force = false) { flush(metadata, force); }

    /// Set the ContentIndex so compaction can propagate index remapping.
    void setContentIndex(std::shared_ptr<ContentIndex> ci) { contentIndex_ = std::move(ci); }

    /// Start a GCD timer that flushes with adaptive interval.
    void startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher);

    /// Stop auto-compaction and wait for in-flight work to finish.
    void stopAutoCompactionAndWait();

    const std::string& basePath() const { return basePath_; }
    const std::string& walPath() const { return walPath_; }

    // Adaptive interval constants
    static constexpr double kBaseIntervalSec = 300.0;
    static constexpr double kMinIntervalSec  = 30.0;
    static constexpr double kMaxIntervalSec  = 600.0;
    static constexpr size_t kWALSizeFlushThreshold = 2 * 1024 * 1024; // 2MB
    static constexpr double kTombstoneCompactRatio = 0.25;
    static constexpr double kDeadSpaceRewriteRatio = 0.5;

private:
    std::shared_ptr<SearchEngine> engine_;
    std::shared_ptr<ContentIndex> contentIndex_;
    std::shared_ptr<IndexWAL> wal_;
    std::unique_ptr<PagedIndexWriter> pagedWriter_;
    std::string basePath_;   // legacy v3 path (index.bin)
    std::string walPath_;
    dispatch_source_t compactionTimer_ = nullptr;
    dispatch_queue_t compactionQueue_ = nullptr;
    std::mutex walMutex_;
    double currentIntervalSec_ = kBaseIntervalSec;

    /// Compute the next auto-compaction interval based on dirty ratio and WAL size.
    double computeAdaptiveInterval() const;

    /// Reschedule the GCD timer with a new interval.
    void rescheduleTimer(double intervalSec);
};
