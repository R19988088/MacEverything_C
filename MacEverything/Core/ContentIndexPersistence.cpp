#include "ContentIndexPersistence.h"
#include "Logger.h"
#include <unistd.h>

// ============================================================
// ContentIndexWAL
// ============================================================

ContentIndexWAL::~ContentIndexWAL() {
    close();
}

bool ContentIndexWAL::open(const std::string& walPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) return false;

    path_ = walPath;
    file_ = fopen(walPath.c_str(), "ab");
    if (!file_) return false;

    // H-3: Write magic+version header if this is a new (empty) file
    long pos = ftell(file_);
    if (pos == 0) {
        uint32_t magic = kMagic;
        uint32_t version = kVersion;
        if (fwrite(&magic, sizeof(uint32_t), 1, file_) != 1 ||
            fwrite(&version, sizeof(uint32_t), 1, file_) != 1) {
            fclose(file_);
            file_ = nullptr;
            return false;
        }
        fflush(file_);
    }

    return true;
}

bool ContentIndexWAL::appendAdd(uint32_t fileIndex, uint64_t contentHash,
                                 const std::vector<Trigram>& trigrams) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    // H-6: Check WAL file size limit
    struct stat st;
    if (fstat(fileno(file_), &st) == 0 && static_cast<size_t>(st.st_size) >= kMaxWALSize) {
        return false;
    }

    // Build entry into buffer for CRC32
    std::string buf;
    uint8_t op = Entry::Add;
    buf.append(reinterpret_cast<const char*>(&op), 1);
    buf.append(reinterpret_cast<const char*>(&fileIndex), sizeof(uint32_t));
    buf.append(reinterpret_cast<const char*>(&contentHash), sizeof(uint64_t));

    uint32_t triCount = static_cast<uint32_t>(trigrams.size());
    buf.append(reinterpret_cast<const char*>(&triCount), sizeof(uint32_t));
    if (triCount > 0) {
        buf.append(reinterpret_cast<const char*>(trigrams.data()), sizeof(Trigram) * triCount);
    }

    // Write entry + CRC32
    if (fwrite(buf.data(), 1, buf.size(), file_) != buf.size()) return false;
    uint32_t checksum = IndexWAL::crc32(buf.data(), buf.size());
    if (fwrite(&checksum, sizeof(uint32_t), 1, file_) != 1) return false;

    fflush(file_);
    unflushedCount_++;

    if (syncInterval_ > 0 && unflushedCount_ >= syncInterval_) {
        fsync(fileno(file_));
        unflushedCount_ = 0;
    }

    return true;
}

bool ContentIndexWAL::appendRemove(uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    // H-6: Check WAL file size limit
    struct stat st;
    if (fstat(fileno(file_), &st) == 0 && static_cast<size_t>(st.st_size) >= kMaxWALSize) {
        return false;
    }

    // Build entry into buffer for CRC32
    std::string buf;
    uint8_t op = Entry::Remove;
    buf.append(reinterpret_cast<const char*>(&op), 1);
    buf.append(reinterpret_cast<const char*>(&fileIndex), sizeof(uint32_t));

    // Write entry + CRC32
    if (fwrite(buf.data(), 1, buf.size(), file_) != buf.size()) return false;
    uint32_t checksum = IndexWAL::crc32(buf.data(), buf.size());
    if (fwrite(&checksum, sizeof(uint32_t), 1, file_) != 1) return false;

    fflush(file_);
    unflushedCount_++;

    if (syncInterval_ > 0 && unflushedCount_ >= syncInterval_) {
        fsync(fileno(file_));
        unflushedCount_ = 0;
    }

    return true;
}

std::vector<ContentIndexWAL::Entry> ContentIndexWAL::readAll(const std::string& walPath) {
    std::vector<Entry> entries;

    FILE* f = fopen(walPath.c_str(), "rb");
    if (!f) return entries;

    // H-3: Verify magic+version header
    uint32_t magic = 0, version = 0;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 ||
        fread(&version, sizeof(uint32_t), 1, f) != 1 ||
        magic != kMagic || version != kVersion) {
        // Legacy WAL without header — try reading from the beginning
        fseek(f, 0, SEEK_SET);
    }

    while (true) {
        long startPos = ftell(f);
        if (startPos < 0) break;

        Entry entry;

        uint8_t opByte;
        if (fread(&opByte, 1, 1, f) != 1) break;
        if (opByte < 1 || opByte > 2) break;
        entry.op = static_cast<Entry::Op>(opByte);

        if (fread(&entry.fileIndex, sizeof(uint32_t), 1, f) != 1) break;

        if (entry.op == Entry::Add) {
            if (fread(&entry.contentHash, sizeof(uint64_t), 1, f) != 1) break;

            uint32_t triCount;
            if (fread(&triCount, sizeof(uint32_t), 1, f) != 1) break;
            if (triCount > 1000000) break; // sanity limit

            entry.trigrams.resize(triCount);
            if (triCount > 0 && fread(entry.trigrams.data(), sizeof(Trigram), triCount, f) != triCount) break;
        }

        // Read and verify CRC32
        long endPos = ftell(f);
        if (endPos < 0) break;
        size_t entryLen = static_cast<size_t>(endPos - startPos);

        uint32_t storedCRC;
        if (fread(&storedCRC, sizeof(uint32_t), 1, f) != 1) break;

        // Re-read entry bytes for CRC computation
        std::vector<uint8_t> rawBuf(entryLen);
        long afterCRC = ftell(f);
        fseek(f, startPos, SEEK_SET);
        if (fread(rawBuf.data(), 1, entryLen, f) != entryLen) break;
        fseek(f, afterCRC, SEEK_SET);

        uint32_t computedCRC = IndexWAL::crc32(rawBuf.data(), rawBuf.size());
        if (computedCRC != storedCRC) {
            // H-4: Log CRC mismatch location for diagnostics
            LOG_ERROR("ContentIndexWAL", "CRC mismatch at offset " << startPos
                      << ", recovered " << entries.size() << " entries");
            break;
        }

        entries.push_back(std::move(entry));
    }

    fclose(f);
    return entries;
}

void ContentIndexWAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ && unflushedCount_ > 0) {
        fsync(fileno(file_));
        unflushedCount_ = 0;
    }
}

void ContentIndexWAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        if (unflushedCount_ > 0) {
            fsync(fileno(file_));
            unflushedCount_ = 0;
        }
        fclose(file_);
        file_ = nullptr;
    }
}

void ContentIndexWAL::closeAndDelete() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        remove(path_.c_str());
    }
}

// ============================================================
// ContentIndexPersistence
// ============================================================

ContentIndexPersistence::ContentIndexPersistence(std::shared_ptr<ContentIndex> index,
                                                 const std::string& basePath,
                                                 const std::string& walPath)
    : index_(std::move(index))
    , basePath_(basePath)
    , walPath_(walPath)
{}

ContentIndexPersistence::~ContentIndexPersistence() {
    stopAutoCompactionAndWait();
    if (wal_) wal_->close();
}

bool ContentIndexPersistence::load() {
    // 1. Load base index
    bool loaded = index_->loadFromFile(basePath_);
    if (loaded) {
        LOG_INFO("ContentIndexPersistence", "Loaded base content index, files="
                  << index_->indexedFileCount());
    } else {
        LOG_INFO("ContentIndexPersistence", "No base content index found at " << basePath_);
    }

    // 2. Replay WAL entries
    auto entries = ContentIndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        LOG_INFO("ContentIndexPersistence", "Replaying " << entries.size() << " content WAL entries");
        for (auto& entry : entries) {
            switch (entry.op) {
                case ContentIndexWAL::Entry::Add:
                    index_->insertFileInfo(entry.fileIndex, entry.contentHash, std::move(entry.trigrams));
                    break;
                case ContentIndexWAL::Entry::Remove:
                    index_->removeFile(entry.fileIndex);
                    break;
            }
        }
        LOG_INFO("ContentIndexPersistence", "Content WAL replay done, indexed files="
                  << index_->indexedFileCount());
    }

    return loaded || !entries.empty();
}

void ContentIndexPersistence::attachWAL() {
    wal_ = std::make_shared<ContentIndexWAL>();
    if (wal_->open(walPath_)) {
        LOG_INFO("ContentIndexPersistence", "Content WAL attached at " << walPath_);
    } else {
        LOG_ERROR("ContentIndexPersistence", "Failed to open content WAL at " << walPath_);
        wal_.reset();
    }
}

void ContentIndexPersistence::compact() {
    // 1. Open a fresh WAL before detaching old one (gap-free swap)
    auto newWal = std::make_shared<ContentIndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        LOG_ERROR("ContentIndexPersistence", "Failed to open new content WAL for compaction");
        return;
    }

    // 2. Swap WAL (under walMutex_ so walAppendAdd/Remove see the new WAL)
    std::shared_ptr<ContentIndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }

    // 3. Write new base file BEFORE deleting old WAL (crash-safety: C-1 fix)
    if (index_->saveToFile(basePath_)) {
        LOG_INFO("ContentIndexPersistence", "Compacted content index, files="
                  << index_->indexedFileCount());
        // 4. Only delete old WAL after base file is safely written
        if (oldWal) {
            oldWal->closeAndDelete();
        }
    } else {
        LOG_ERROR("ContentIndexPersistence", "Failed to write content base index"
                  << " — keeping old WAL for recovery");
        // Keep old WAL alive for crash recovery; close without deleting
        if (oldWal) {
            oldWal->close();
        }
    }

    // 5. Rename new WAL to standard path
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        LOG_ERROR("ContentIndexPersistence", "Failed to rename WAL: " << newWalPath << " -> " << walPath_);
    }
}

void ContentIndexPersistence::startAutoCompaction(double intervalSec) {
    stopAutoCompactionAndWait();

    // Use a dedicated serial queue so we can dispatch_sync to drain in-flight work
    compactionQueue_ = dispatch_queue_create("com.maceverything.content.compaction", DISPATCH_QUEUE_SERIAL);
    compactionTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, compactionQueue_);
    uint64_t intervalNs = static_cast<uint64_t>(intervalSec * NSEC_PER_SEC);
    dispatch_source_set_timer(compactionTimer_,
                              dispatch_time(DISPATCH_TIME_NOW, intervalNs),
                              intervalNs,
                              30 * NSEC_PER_SEC);

    auto* self = this;
    dispatch_source_set_event_handler(compactionTimer_, ^{
        self->compact();
    });

    dispatch_resume(compactionTimer_);
    LOG_INFO("ContentIndexPersistence", "Auto-compaction started (every " << intervalSec << "s)");
}

void ContentIndexPersistence::stopAutoCompactionAndWait() {
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

void ContentIndexPersistence::walAppendAdd(uint32_t fileIndex, uint64_t contentHash,
                                            const std::vector<Trigram>& trigrams) {
    std::lock_guard<std::mutex> lock(walMutex_);
    if (wal_) {
        wal_->appendAdd(fileIndex, contentHash, trigrams);
    }
}

void ContentIndexPersistence::walAppendRemove(uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(walMutex_);
    if (wal_) {
        wal_->appendRemove(fileIndex);
    }
}
