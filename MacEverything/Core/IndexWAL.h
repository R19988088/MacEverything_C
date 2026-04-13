#pragma once
#include "FileRecord.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdint>

enum class WALOp : uint8_t {
    Add    = 1,
    Remove = 2,
    Update = 3
};

struct WALEntry {
    WALOp op;
    std::string fullPath;
    FileRecord record; // populated for Add/Update, empty for Remove
};

class IndexWAL {
public:
    IndexWAL() = default;
    ~IndexWAL();

    IndexWAL(const IndexWAL&) = delete;
    IndexWAL& operator=(const IndexWAL&) = delete;

    /// Open WAL file for appending. Creates if not exists.
    bool open(const std::string& walPath);

    /// Append a mutation entry. Thread-safe. Does NOT fsync on each call.
    bool append(WALOp op, const std::string& fullPath, const FileRecord& record = {});

    /// Explicitly flush buffered writes to disk (fflush + fsync).
    void flush();

    /// Read all valid entries from a WAL file (stops at first corrupt entry).
    static std::vector<WALEntry> readAll(const std::string& walPath);

    /// Number of entries written since open.
    uint64_t entryCount() const { return entryCount_; }

    /// Close the WAL file.
    void close();

    /// Close and delete the WAL file.
    void closeAndDelete();

    bool isOpen() const { return file_ != nullptr; }

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint64_t entryCount_ = 0;
    std::mutex mutex_;

    static bool writeRecord(FILE* f, const FileRecord& record);
    static bool readRecord(FILE* f, FileRecord& record);
};
