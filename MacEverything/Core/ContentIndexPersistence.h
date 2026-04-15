#pragma once
#include "ContentIndex.h"
#include "IndexWAL.h" // for IndexWAL::crc32()
#include <string>
#include <memory>
#include <mutex>
#include <cstdio>
#include <sys/stat.h>
#include <dispatch/dispatch.h>

/// WAL for content index mutations.
class ContentIndexWAL {
public:
    ContentIndexWAL() = default;
    ~ContentIndexWAL();

    ContentIndexWAL(const ContentIndexWAL&) = delete;
    ContentIndexWAL& operator=(const ContentIndexWAL&) = delete;

    /// Open WAL file for appending.
    bool open(const std::string& walPath);

    /// Append an add entry: fileIndex + contentHash + trigrams.
    bool appendAdd(uint32_t fileIndex, uint64_t contentHash, const std::vector<Trigram>& trigrams);

    /// Append a remove entry: fileIndex.
    bool appendRemove(uint32_t fileIndex);

    /// Read all valid entries from a WAL file.
    struct Entry {
        enum Op : uint8_t { Add = 1, Remove = 2 };
        Op op;
        uint32_t fileIndex;
        uint64_t contentHash; // only for Add
        std::vector<Trigram> trigrams; // only for Add
    };
    static std::vector<Entry> readAll(const std::string& walPath);

    /// Force fsync to disk. Call when you need durability guarantees.
    void sync();

    /// Set the number of entries between automatic fsyncs (0 = never auto-fsync).
    void setSyncInterval(uint64_t entries) { syncInterval_ = entries; }

    void close();
    void closeAndDelete();
    bool isOpen() const { return file_ != nullptr; }

    /// Update the internal path after an external rename (e.g., compact rename).
    void updatePath(const std::string& newPath);

    /// Maximum WAL file size (20 MB).
    static constexpr size_t kMaxWALSize = 20 * 1024 * 1024;

    /// H-3: WAL file header constants
    static constexpr uint32_t kMagic   = 0x43574C31; // "CWL1"
    static constexpr uint32_t kVersion = 1;

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint64_t unflushedCount_ = 0;
    uint64_t syncInterval_ = 64;  // fsync every 64 entries by default
    std::mutex mutex_;
};

/// Orchestrates content index persistence: base file + WAL + auto-compaction.
class ContentIndexPersistence {
public:
    ContentIndexPersistence(std::shared_ptr<ContentIndex> index,
                            const std::string& basePath,
                            const std::string& walPath);
    ~ContentIndexPersistence();

    ContentIndexPersistence(const ContentIndexPersistence&) = delete;
    ContentIndexPersistence& operator=(const ContentIndexPersistence&) = delete;

    /// Load base index + replay WAL entries.
    bool load();

    /// Start logging mutations to WAL.
    void attachWAL();

    /// Compact: write new base, clear WAL.
    void compact();

    /// Start a GCD timer that compacts every intervalSec seconds.
    void startAutoCompaction(double intervalSec);

    /// Stop auto-compaction and wait for in-flight compaction to finish.
    void stopAutoCompactionAndWait();

    /// WAL accessors for the bridge layer to log mutations.
    void walAppendAdd(uint32_t fileIndex, uint64_t contentHash, const std::vector<Trigram>& trigrams);
    void walAppendRemove(uint32_t fileIndex);

    const std::string& basePath() const { return basePath_; }
    const std::string& walPath() const { return walPath_; }

private:
    std::shared_ptr<ContentIndex> index_;
    std::shared_ptr<ContentIndexWAL> wal_;
    std::string basePath_;
    std::string walPath_;
    std::mutex walMutex_;
    dispatch_source_t compactionTimer_ = nullptr;
    dispatch_queue_t compactionQueue_ = nullptr;
};
