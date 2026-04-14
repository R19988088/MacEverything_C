#pragma once
#include "SearchEngine.h"
#include "ContentIndex.h"
#include "IndexWAL.h"
#include "FileSystemWatcher.h"
#include <string>
#include <memory>
#include <mutex>
#include <dispatch/dispatch.h>

/// Orchestrates index persistence: base file + WAL + auto-compaction.
class IndexPersistence {
public:
    IndexPersistence(std::shared_ptr<SearchEngine> engine,
                     const std::string& basePath,
                     const std::string& walPath);
    ~IndexPersistence();

    IndexPersistence(const IndexPersistence&) = delete;
    IndexPersistence& operator=(const IndexPersistence&) = delete;

    /// Load base index + replay WAL entries.
    /// Returns the lastEventId from the base file (0 if no file or v1 format).
    uint64_t load();

    /// Start logging mutations to WAL.
    void attachWAL();

    /// Write a new base snapshot with metadata, clear WAL.
    void compact(uint64_t lastEventId);

    /// Write a new base snapshot with full metadata, clear WAL.
    void compact(const IndexMetadata& metadata);

    /// Set the ContentIndex so compaction can propagate index remapping.
    void setContentIndex(std::shared_ptr<ContentIndex> ci) { contentIndex_ = std::move(ci); }

    /// Start a GCD timer that compacts every `intervalSec` seconds.
    /// H9: Takes shared_ptr to prevent use-after-free if watcher is destroyed before timer fires.
    void startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher);

    /// Stop auto-compaction and wait for in-flight compaction to finish.
    void stopAutoCompactionAndWait();

    const std::string& basePath() const { return basePath_; }
    const std::string& walPath() const { return walPath_; }

private:
    std::shared_ptr<SearchEngine> engine_;
    std::shared_ptr<ContentIndex> contentIndex_;
    std::shared_ptr<IndexWAL> wal_;
    std::string basePath_;
    std::string walPath_;
    dispatch_source_t compactionTimer_ = nullptr;
    dispatch_queue_t compactionQueue_ = nullptr;
    std::mutex walMutex_;
};
