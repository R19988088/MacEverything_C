#pragma once
#include "FileRecord.h"
#include "ContentIndex.h" // for Trigram type and extractTrigrams/makeTrigram
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <map>

class IndexWAL;

/// Metadata stored in the index header (v3+). Extensible key-value pairs.
struct IndexMetadata {
    uint32_t formatVersion = 0; // populated on load
    int64_t  timestamp = 0;     // creation timestamp (seconds since epoch)
    uint64_t lastEventId = 0;   // FSEvents stream ID for incremental replay
    std::map<std::string, std::string> extra; // extensible key-value pairs

    // Well-known metadata keys
    static constexpr const char* kScanRoot     = "scan_root";      // e.g. "/"
    static constexpr const char* kAppVersion   = "app_version";    // e.g. "1.0.0"
    static constexpr const char* kOSVersion    = "os_version";     // e.g. "14.5"
    static constexpr const char* kRecordFormat = "record_format";  // e.g. "v2_inode"
};

class SearchEngine {
public:
    /// Takes ownership of scanned records and pre-computes lowercase names.
    void loadRecords(std::vector<FileRecord>&& records);

    /// Case-insensitive substring search. Returns indices into the records array.
    /// If maxResults > 0, stops early once enough matches are found.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults = 0) const;

    FileRecord getRecord(uint32_t index) const;
    uint32_t recordCount() const;

    // --- Incremental mutation (for FSEvents) ---

    /// Append a new record. Returns the new index. Thread-safe.
    uint32_t addRecord(FileRecord&& record);

    /// Mark record at fullPath as deleted (tombstone). Thread-safe.
    /// Returns true if found and removed.
    bool removeByPath(const std::string& fullPath);

    /// Remove old record at fullPath (if any) and add updated record. Thread-safe.
    void updateByPath(const std::string& fullPath, FileRecord&& updated);

    /// Remove all records whose full path starts with the given prefix. Thread-safe.
    /// Returns the number of records removed.
    uint32_t removeByPathPrefix(const std::string& pathPrefix);

    /// Number of live (non-tombstoned) records.
    uint32_t liveRecordCount() const { return liveCount_.load(std::memory_order_relaxed); }

    /// Compact in-memory records by removing tombstoned entries.
    /// Rebuilds pathIndex_ and lowerNames_. Thread-safe.
    /// Returns a mapping from old index → new index for live records.
    /// Returns empty map if nothing was compacted.
    std::unordered_map<uint32_t, uint32_t> compactRecords();

    /// Save live records to a binary file (format v3). Thread-safe.
    /// metadata.extra can contain arbitrary key-value pairs for forward compatibility.
    bool saveToFile(const std::string& filePath, const IndexMetadata& metadata) const;

    /// Legacy overload for backward compatibility.
    bool saveToFile(const std::string& filePath, uint64_t lastEventId = 0) const;

    /// Load records from a binary file (v1, v2, or v3), rebuilding indices. Thread-safe.
    /// outMetadata receives all metadata including formatVersion, timestamp, lastEventId, and extra pairs.
    bool loadFromFile(const std::string& filePath, IndexMetadata* outMetadata = nullptr);

    /// Legacy overload for backward compatibility.
    bool loadFromFile(const std::string& filePath, uint64_t* outLastEventId);

    /// Attach a WAL for logging mutations. Must be already opened.
    void attachWAL(std::shared_ptr<IndexWAL> wal);

    /// Detach the WAL (e.g. before compaction).
    void detachWAL();

    /// Monotonically increasing generation counter, incremented on each compaction.
    uint64_t compactionGeneration() const { return compactionGen_.load(std::memory_order_relaxed); }

    /// Return indices of the most recently modified live records, sorted by modTime descending.
    /// Accesses records_ directly under the lock to avoid per-record copy overhead.
    std::vector<uint32_t> recentIndices(uint32_t count) const;

    /// Look up the record index for a given full path. Returns UINT32_MAX if not found.
    uint32_t indexForPath(const std::string& fullPath) const;

    /// Build the full path from a record's path and name components.
    static std::string makeFullPath(const std::string& path, const std::string& name);

private:
    std::vector<FileRecord> records_;
    std::vector<std::string> lowerNames_; // pre-computed lowercase filenames
    std::unordered_map<std::string, uint32_t> pathIndex_; // fullPath -> index
    std::atomic<uint32_t> liveCount_{0};
    mutable std::shared_mutex mutex_;

    // Trigram inverted index for fast filename search
    std::unordered_map<Trigram, std::vector<uint32_t>> nameTrigramIndex_; // trigram -> record indices
    std::vector<std::vector<Trigram>> recordTrigrams_; // per-record trigram list (for removal)

    /// Build trigram index from lowerNames_ (called inside loadRecords/compactRecords under lock)
    void buildTrigramIndex();
    /// Add trigrams for a single record to the index
    void addTrigramsForRecord(uint32_t idx, const std::string& lowerName);
    /// Remove trigrams for a single record from the index
    void removeTrigramsForRecord(uint32_t idx);

    static std::string toLower(const std::string& s);
    static bool isGlobPattern(const std::string& s);
    static bool globMatch(const std::string& pattern, const std::string& text);

    std::shared_ptr<IndexWAL> wal_;
    std::atomic<uint64_t> compactionGen_{0};
};
