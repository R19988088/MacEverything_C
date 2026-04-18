#pragma once
#include "FileRecord.h"
#include "ContentIndex.h" // for Trigram type and extractTrigrams/makeTrigram
#include "StringPool.h"
#include "SIMDSearch.h"
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <map>
#include <set>

#include "IndexWAL.h"

/// Deduplication table for directory path strings.
/// Interns unique paths and returns a compact uint32_t index.
class PathTable {
public:
    /// Look up or insert a path, returning its index.
    uint32_t intern(const std::string& path) {
        auto it = lookup_.find(path);
        if (it != lookup_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(paths_.size());
        paths_.push_back(path);
        lookup_[path] = idx;
        return idx;
    }

    /// Resolve an index back to its path string.
    /// Returns empty string if index is out of range.
    const std::string& resolve(uint32_t index) const {
        static const std::string empty;
        if (index >= paths_.size()) return empty;
        return paths_[index];
    }

    /// Number of unique paths stored.
    uint32_t size() const { return static_cast<uint32_t>(paths_.size()); }

    /// Clear all entries.
    void clear() { paths_.clear(); lookup_.clear(); }

    PathTable() = default;
    PathTable(PathTable&&) = default;
    PathTable& operator=(PathTable&&) = default;
    PathTable(const PathTable&) = default;
    PathTable& operator=(const PathTable&) = default;

private:
    std::vector<std::string> paths_;                    // index -> path
    std::unordered_map<std::string, uint32_t> lookup_;  // path -> index
};

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

/// Detailed timing breakdown from a search query.
struct QueryTimingInfo {
    double totalMs = 0;
    double lockWaitMs = 0;
    double trigramMs = 0;     // trigram index lookup (trigram path only)
    double phase1Ms = 0;      // name verification (trigram path only)
    double phase2Ms = 0;      // path-only match / NEON full scan
    double sortMs = 0;
    double lockHeldMs = 0;
    size_t totalRecords = 0;
    size_t candidates = 0;    // trigram candidates count
    size_t nameMatches = 0;   // phase1 results (name matches)
    size_t pathMatches = 0;   // phase2 results (path-only matches)
    size_t resultCount = 0;
    bool usedTrigram = false;
    std::string searchPath;   // "trigram", "trigram-split", or "linear"
};

class SearchEngine {
public:
    /// Takes ownership of scanned records and pre-computes lowercase names.
    void loadRecords(std::vector<FileRecord>&& records);

    /// v5 fast-path load: accepts pre-separated data from on-disk path dictionary.
    /// Skips toLower of names and path dedup since they are pre-computed.
    void loadRecordsV5(std::vector<FileRecord>&& records,
                       std::vector<std::string>&& lowerNames,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathDict,
                       StringPool&& lowerPathDict);

    /// Case-insensitive substring search. Returns indices into the records array.
    /// If maxResults > 0, stops early once enough matches are found.
    /// If useTrigram is false, bypasses trigram index and does NEON full scan.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults = 0,
                                bool useTrigram = true) const;

    /// Same as query() but also populates timing breakdown.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults,
                                bool useTrigram, QueryTimingInfo& timing) const;

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

    /// Batch-replace all records under pathPrefix with freshRecords.
    /// Tombstones old records, adds fresh records, and rebuilds trigram index once.
    /// Returns the number of old records removed. Thread-safe.
    uint32_t batchRescanPrefix(const std::string& pathPrefix,
                               std::vector<FileRecord>&& freshRecords);

    /// Replay WAL entries in-place using pathIndex_ for O(1) lookups.
    /// Skips WAL writes (entries came FROM the WAL). Thread-safe.
    void replayWALEntries(std::vector<WALEntry>&& entries);

    /// Number of live (non-tombstoned) records.
    uint32_t liveRecordCount() const { return liveCount_.load(std::memory_order_relaxed); }

    /// Compact in-memory records by removing tombstoned entries.
    /// Rebuilds pathIndex_ and namePool_. Thread-safe.
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

    /// Records per page for paged persistence
    static constexpr uint32_t kRecordsPerPage = 1024;

    /// Get the list of dirty page numbers (pages modified since last clearDirtyPages). Thread-safe.
    std::vector<uint32_t> getDirtyPageNumbers() const;

    /// Clear the dirty page bitmap. Thread-safe.
    void clearDirtyPages();

    /// Whether a full rewrite is needed (set after compactRecords renumbers indices).
    bool needsFullRewrite() const { return fullRewriteNeeded_.load(std::memory_order_relaxed); }
    void clearFullRewriteNeeded() { fullRewriteNeeded_.store(false, std::memory_order_relaxed); }

    /// Monotonically increasing generation counter, incremented on each compaction.
    uint64_t compactionGeneration() const { return compactionGen_.load(std::memory_order_relaxed); }

    /// Return indices of the most recently modified live records, sorted by modTime descending.
    /// Accesses records_ directly under the lock to avoid per-record copy overhead.
    std::vector<uint32_t> recentIndices(uint32_t count) const;

    /// Look up the record index for a given full path. Returns UINT32_MAX if not found.
    uint32_t indexForPath(const std::string& fullPath) const;

    /// Export a copy of all live records with their paths restored. Thread-safe.
    std::vector<FileRecord> exportRecords() const;

    /// Build the full path from a record's path and name components.
    static std::string makeFullPath(const std::string& path, const std::string& name);

    /// Resolve a record's path via pathPool_. Thread-safe (acquires shared_lock).
    /// Returns by value to prevent dangling references during concurrent compaction.
    std::string resolveRecordPath(uint32_t index) const {
        std::shared_lock lock(mutex_);
        if (index >= pathIndices_.size()) return {};
        return pathPool_.str(pathIndices_[index]);
    }

    /// Batch callback access under a single shared_lock. Avoids per-record copy.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordWithPath(const std::vector<uint32_t>& indices, Func&& func) const {
        std::shared_lock lock(mutex_);
        for (uint32_t idx : indices) {
            if (idx >= records_.size() || records_[idx].type == 0) continue;
            std::string path = pathPool_.str(pathIndices_[idx]);
            func(idx, records_[idx], path);
        }
    }

    /// Batch callback for a contiguous range of records, including tombstones.
    /// Used by PagedIndexWriter to serialize pages with stable index positions.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordInRange(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(records_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            std::string path = pathPool_.str(pathIndices_[i]);
            func(i, records_[i], path);
        }
    }

    /// Batch callback for v5 serialization. Provides pathIdx and lower name data directly.
    /// Callback signature: void(uint32_t idx, const FileRecord& rec, uint32_t pathIdx,
    ///                          const char* lowerName, uint16_t lowerNameLen)
    template<typename Func>
    void forEachRecordInRangeV5(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(records_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            func(i, records_[i], pathIndices_[i],
                 namePool_.data(i), namePool_.length(i));
        }
    }

    /// Thread-safe copy of pathPool_ for serialization.
    StringPool pathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return pathPool_;
    }

    /// Thread-safe copy of lowerPathPool_ for serialization.
    StringPool lowerPathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return lowerPathPool_;
    }

    /// Thread-safe snapshot of PathTable. Reconstructs from pathPool_ for compatibility.
    PathTable pathTableSnapshot() const {
        std::shared_lock lock(mutex_);
        PathTable pt;
        for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
            if (pathPool_.isLive(i)) {
                pt.intern(pathPool_.str(i));
            }
        }
        return pt;
    }

private:
    std::vector<FileRecord> records_;
    StringPool namePool_;                  // contiguous lowercase filenames
    std::vector<uint32_t> pathIndices_;    // per-record index into pathPool_
    StringPool pathPool_;                  // contiguous directory paths (deduplicated)
    StringPool lowerPathPool_;             // parallel to pathPool_, stores pre-lowered paths
    std::unordered_map<std::string, uint32_t> pathLookup_; // path string -> pathPool_ index
    std::unordered_map<std::string, uint32_t> pathIndex_; // fullPath -> record index
    std::atomic<uint32_t> liveCount_{0};
    mutable std::shared_mutex mutex_;

    // Trigram inverted index for fast filename search
    std::unordered_map<Trigram, std::vector<uint32_t>> nameTrigramIndex_; // trigram -> record indices

    // Path trigram index: two-level lookup for fast path-only matching
    std::unordered_map<Trigram, std::vector<uint32_t>> pathTrigramIndex_; // trigram -> sorted pathIdx
    std::vector<std::vector<uint32_t>> pathIdxToRecords_; // pathIdx -> sorted record indices

    /// Intern a directory path into pathPool_ and lowerPathPool_. Returns pathPool_ index.
    /// Deduplicates via pathLookup_. Must be called under unique_lock.
    uint32_t internPath(const std::string& path);

    /// Build trigram index from namePool_ (called inside loadRecords/compactRecords under lock)
    void buildTrigramIndex();
    /// Add trigrams for a single record to the index
    void addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len);
    /// Remove trigrams for a single record from the index
    void removeTrigramsForRecord(uint32_t idx);

    /// Build path trigram index from pathPool_
    void buildPathTrigramIndex();
    /// Rebuild pathIdx -> record indices mapping
    void rebuildPathIdxToRecords();
    /// Add a record to pathIdxToRecords_ (sorted insert)
    void addPathTrigramsForRecord(uint32_t idx);
    /// Remove a record from pathIdxToRecords_ (sorted remove)
    void removePathTrigramsForRecord(uint32_t idx);
    /// Ensure pathTrigramIndex_ has entries for a given pathIdx (called when PathTable grows)
    void ensurePathTrigramsForPathIdx(uint32_t pathIdx);

    static bool isGlobPattern(const std::string& s);
    static bool globMatch(const std::string& pattern, const std::string& text);

    std::vector<bool> dirtyPages_;                // dirty page bitmap
    std::atomic<bool> fullRewriteNeeded_{false};   // set by compactRecords()

    /// Mark the page containing the given record index as dirty. Must be called under lock.
    void markPageDirty(uint32_t recordIndex);

    std::shared_ptr<IndexWAL> wal_;
    std::atomic<uint64_t> compactionGen_{0};

    // Query cancellation: incremented on each query(), checked in scan loops
    mutable std::atomic<uint64_t> queryGeneration_{0};

    // Recent files cache: top-K records by modTime, maintained incrementally
    static constexpr uint32_t kRecentCacheSize = 200;
    struct RecentEntry {
        time_t modTime;
        uint32_t index;
        bool operator<(const RecentEntry& o) const {
            return modTime > o.modTime || (modTime == o.modTime && index > o.index);
        }
    };
    std::set<RecentEntry> recentCache_;
    void rebuildRecentCache();
    void addToRecentCache(uint32_t idx, time_t modTime);
    void removeFromRecentCache(uint32_t idx, time_t modTime);

    /// Build trigram index from standalone data (no member access, used by COW compaction)
    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildTrigramIndexFromData(const std::vector<FileRecord>& records,
                                  const StringPool& namePool);

    /// Build path trigram index from pre-lowered StringPool (used by COW compaction)
    static std::unordered_map<Trigram, std::vector<uint32_t>>
        buildPathTrigramIndexFromData(const StringPool& lowerPathPool);

    /// Build pathIdx -> record indices mapping from standalone data (used by COW compaction)
    static std::vector<std::vector<uint32_t>>
        buildPathIdxToRecordsFromData(const std::vector<FileRecord>& records,
                                      const std::vector<uint32_t>& pathIndices,
                                      uint32_t pathPoolSize);

    /// Build recent cache from standalone data (no member access, used by COW compaction)
    static std::set<RecentEntry>
        buildRecentCacheFromData(const std::vector<FileRecord>& records,
                                 uint32_t cacheSize);
};
