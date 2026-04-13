#include "IndexWAL.h"
#include <cstring>
#include <unistd.h>

IndexWAL::~IndexWAL() {
    close();
}

bool IndexWAL::open(const std::string& walPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) return false;

    path_ = walPath;
    file_ = fopen(walPath.c_str(), "ab");
    if (!file_) return false;

    entryCount_ = 0;
    return true;
}

bool IndexWAL::append(WALOp op, const std::string& fullPath, const FileRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return false;

    // Entry format: op(1) + pathLen(4) + path + [record fields if Add/Update]
    uint8_t opByte = static_cast<uint8_t>(op);
    if (fwrite(&opByte, 1, 1, file_) != 1) return false;

    uint32_t pathLen = static_cast<uint32_t>(fullPath.size());
    if (fwrite(&pathLen, sizeof(uint32_t), 1, file_) != 1) return false;
    if (pathLen > 0 && fwrite(fullPath.data(), 1, pathLen, file_) != pathLen) return false;

    if (op == WALOp::Add || op == WALOp::Update) {
        if (!writeRecord(file_, record)) return false;
    }

    entryCount_++;
    return true;
}

void IndexWAL::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fflush(file_);
        fsync(fileno(file_));
    }
}

std::vector<WALEntry> IndexWAL::readAll(const std::string& walPath) {
    std::vector<WALEntry> entries;

    FILE* f = fopen(walPath.c_str(), "rb");
    if (!f) return entries;

    while (true) {
        WALEntry entry;

        uint8_t opByte;
        if (fread(&opByte, 1, 1, f) != 1) break;
        if (opByte < 1 || opByte > 3) break; // invalid op
        entry.op = static_cast<WALOp>(opByte);

        uint32_t pathLen;
        if (fread(&pathLen, sizeof(uint32_t), 1, f) != 1) break;
        if (pathLen > 65536) break; // sanity limit
        entry.fullPath.resize(pathLen);
        if (pathLen > 0 && fread(entry.fullPath.data(), 1, pathLen, f) != pathLen) break;

        if (entry.op == WALOp::Add || entry.op == WALOp::Update) {
            if (!readRecord(f, entry.record)) break;
        }

        entries.push_back(std::move(entry));
    }

    fclose(f);
    return entries;
}

void IndexWAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

void IndexWAL::closeAndDelete() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        remove(path_.c_str());
    }
}

bool IndexWAL::writeRecord(FILE* f, const FileRecord& r) {
    uint32_t nameLen = static_cast<uint32_t>(r.name.size());
    if (fwrite(&nameLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (nameLen > 0 && fwrite(r.name.data(), 1, nameLen, f) != nameLen) return false;

    uint32_t pathLen = static_cast<uint32_t>(r.path.size());
    if (fwrite(&pathLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (pathLen > 0 && fwrite(r.path.data(), 1, pathLen, f) != pathLen) return false;

    if (fwrite(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fwrite(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod = static_cast<int64_t>(r.modTime);
    if (fwrite(&mod, sizeof(int64_t), 1, f) != 1) return false;
    if (fwrite(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
    if (fwrite(&r.devId, sizeof(int32_t), 1, f) != 1) return false;

    return true;
}

bool IndexWAL::readRecord(FILE* f, FileRecord& r) {
    uint32_t nameLen;
    if (fread(&nameLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (nameLen > 65536) return false;
    r.name.resize(nameLen);
    if (nameLen > 0 && fread(r.name.data(), 1, nameLen, f) != nameLen) return false;

    uint32_t pathLen;
    if (fread(&pathLen, sizeof(uint32_t), 1, f) != 1) return false;
    if (pathLen > 65536) return false;
    r.path.resize(pathLen);
    if (pathLen > 0 && fread(r.path.data(), 1, pathLen, f) != pathLen) return false;

    if (fread(&r.type, sizeof(uint8_t), 1, f) != 1) return false;
    if (fread(&r.size, sizeof(uint64_t), 1, f) != 1) return false;
    int64_t mod;
    if (fread(&mod, sizeof(int64_t), 1, f) != 1) return false;
    r.modTime = static_cast<time_t>(mod);
    if (fread(&r.inode, sizeof(uint64_t), 1, f) != 1) return false;
    if (fread(&r.devId, sizeof(int32_t), 1, f) != 1) return false;

    return true;
}
