#include "ContentIndexPersistence.h"
#include <iostream>
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
    return file_ != nullptr;
}

bool ContentIndexWAL::appendAdd(uint32_t fileIndex, uint64_t contentHash,
                                 const std::vector<Trigram>& trigrams) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    uint8_t op = Entry::Add;
    if (fwrite(&op, 1, 1, file_) != 1) return false;
    if (fwrite(&fileIndex, sizeof(uint32_t), 1, file_) != 1) return false;
    if (fwrite(&contentHash, sizeof(uint64_t), 1, file_) != 1) return false;

    uint32_t triCount = static_cast<uint32_t>(trigrams.size());
    if (fwrite(&triCount, sizeof(uint32_t), 1, file_) != 1) return false;
    if (triCount > 0 && fwrite(trigrams.data(), sizeof(Trigram), triCount, file_) != triCount) return false;

    fflush(file_);
    fsync(fileno(file_));
    return true;
}

bool ContentIndexWAL::appendRemove(uint32_t fileIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    uint8_t op = Entry::Remove;
    if (fwrite(&op, 1, 1, file_) != 1) return false;
    if (fwrite(&fileIndex, sizeof(uint32_t), 1, file_) != 1) return false;

    fflush(file_);
    fsync(fileno(file_));
    return true;
}

std::vector<ContentIndexWAL::Entry> ContentIndexWAL::readAll(const std::string& walPath) {
    std::vector<Entry> entries;

    FILE* f = fopen(walPath.c_str(), "rb");
    if (!f) return entries;

    while (true) {
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

        entries.push_back(std::move(entry));
    }

    fclose(f);
    return entries;
}

void ContentIndexWAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
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
        std::cout << "[ContentIndexPersistence] Loaded base content index, files="
                  << index_->indexedFileCount() << "\n";
    } else {
        std::cout << "[ContentIndexPersistence] No base content index found at " << basePath_ << "\n";
    }

    // 2. Replay WAL entries
    auto entries = ContentIndexWAL::readAll(walPath_);
    if (!entries.empty()) {
        std::cout << "[ContentIndexPersistence] Replaying " << entries.size() << " content WAL entries\n";
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
        std::cout << "[ContentIndexPersistence] Content WAL replay done, indexed files="
                  << index_->indexedFileCount() << "\n";
    }

    return loaded || !entries.empty();
}

void ContentIndexPersistence::attachWAL() {
    wal_ = std::make_shared<ContentIndexWAL>();
    if (wal_->open(walPath_)) {
        std::cout << "[ContentIndexPersistence] Content WAL attached at " << walPath_ << "\n";
    } else {
        std::cerr << "[ContentIndexPersistence] Failed to open content WAL at " << walPath_ << "\n";
        wal_.reset();
    }
}

void ContentIndexPersistence::compact() {
    // 1. Open a fresh WAL before detaching old one (gap-free swap)
    auto newWal = std::make_shared<ContentIndexWAL>();
    std::string newWalPath = walPath_ + ".new";
    if (!newWal->open(newWalPath)) {
        std::cerr << "[ContentIndexPersistence] Failed to open new content WAL for compaction\n";
        return;
    }

    // 2. Swap WAL (under walMutex_ so walAppendAdd/Remove see the new WAL)
    std::shared_ptr<ContentIndexWAL> oldWal;
    {
        std::lock_guard<std::mutex> lock(walMutex_);
        oldWal = wal_;
        wal_ = newWal;
    }

    // 3. Close and delete old WAL
    if (oldWal) {
        oldWal->closeAndDelete();
    }

    // 4. Write new base file
    if (index_->saveToFile(basePath_)) {
        std::cout << "[ContentIndexPersistence] Compacted content index, files="
                  << index_->indexedFileCount() << "\n";
    } else {
        std::cerr << "[ContentIndexPersistence] Failed to write content base index\n";
    }

    // 5. Rename new WAL to standard path
    if (rename(newWalPath.c_str(), walPath_.c_str()) != 0) {
        std::cerr << "[ContentIndexPersistence] Failed to rename WAL: " << newWalPath << " -> " << walPath_ << "\n";
    }
}

void ContentIndexPersistence::startAutoCompaction(double intervalSec) {
    stopAutoCompaction();

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
    std::cout << "[ContentIndexPersistence] Auto-compaction started (every " << intervalSec << "s)\n";
}

void ContentIndexPersistence::stopAutoCompaction() {
    if (compactionTimer_) {
        dispatch_source_cancel(compactionTimer_);
        dispatch_release(compactionTimer_);
        compactionTimer_ = nullptr;
    }
    if (compactionQueue_) {
        dispatch_release(compactionQueue_);
        compactionQueue_ = nullptr;
    }
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
