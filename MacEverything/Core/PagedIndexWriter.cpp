#include "PagedIndexWriter.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>

// ─── Helpers: same format as SearchEnginePersistence ───

static bool writeString(FILE* f, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    if (fwrite(&len, sizeof(uint32_t), 1, f) != 1) return false;
    if (len > 0 && fwrite(s.data(), 1, len, f) != len) return false;
    return true;
}

static bool readString(const uint8_t*& p, const uint8_t* end, std::string& s) {
    if (p + 4 > end) return false;
    uint32_t len;
    memcpy(&len, p, 4); p += 4;
    if (len > 65536) return false;
    if (p + len > end) return false;
    s.assign(reinterpret_cast<const char*>(p), len);
    p += len;
    return true;
}

static bool readRecordFromBuf(const uint8_t*& p, const uint8_t* end, FileRecord& r) {
    if (!readString(p, end, r.name)) return false;
    if (!readString(p, end, r.path)) return false;
    if (p + 1 > end) return false;
    r.type = *p++;
    if (p + 8 > end) return false;
    memcpy(&r.size, p, 8); p += 8;
    if (p + 8 > end) return false;
    int64_t mod;
    memcpy(&mod, p, 8); p += 8;
    r.modTime = static_cast<time_t>(mod);
    if (p + 8 > end) return false;
    memcpy(&r.inode, p, 8); p += 8;
    if (p + 4 > end) return false;
    memcpy(&r.devId, p, 4); p += 4;
    return true;
}

// ─── Serialization to in-memory buffer ───

static void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&v),
               reinterpret_cast<uint8_t*>(&v) + 4);
}

static void appendString(std::vector<uint8_t>& buf, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    appendU32(buf, len);
    buf.insert(buf.end(), s.begin(), s.end());
}

static void appendRecord(std::vector<uint8_t>& buf, const FileRecord& r,
                          const std::string& resolvedPath) {
    appendString(buf, r.name);
    appendString(buf, resolvedPath);
    buf.push_back(r.type);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&r.size),
               reinterpret_cast<const uint8_t*>(&r.size) + 8);
    int64_t mod = static_cast<int64_t>(r.modTime);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&mod),
               reinterpret_cast<const uint8_t*>(&mod) + 8);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&r.inode),
               reinterpret_cast<const uint8_t*>(&r.inode) + 8);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&r.devId),
               reinterpret_cast<const uint8_t*>(&r.devId) + 4);
}

// ─── PagedIndexWriter ───

PagedIndexWriter::PagedIndexWriter(const std::string& pagesPath, const std::string& ptablePath)
    : pagesPath_(pagesPath), ptablePath_(ptablePath) {}

bool PagedIndexWriter::exists() const {
    return access(ptablePath_.c_str(), R_OK) == 0 &&
           access(pagesPath_.c_str(), R_OK) == 0;
}

// ─── deserializePage ───

bool PagedIndexWriter::deserializePage(
    const uint8_t* data, size_t len,
    uint16_t expectedCount,
    std::vector<FileRecord>& out)
{
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    out.reserve(expectedCount);
    for (uint16_t i = 0; i < expectedCount; i++) {
        FileRecord r;
        if (!readRecordFromBuf(p, end, r)) return false;
        out.push_back(std::move(r));
    }
    return true;
}

// ─── writePtable ───

bool PagedIndexWriter::writePtable(const IndexMetadata& meta, uint32_t totalRecords) const {
    std::string tmpPath = ptablePath_ + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return false;

    bool ok = true;
    auto safeWrite = [&](const void* ptr, size_t size, size_t count) {
        if (ok && fwrite(ptr, size, count, f) != count) ok = false;
    };

    // Header
    uint32_t magic = kPtableMagic;
    safeWrite(&magic, 4, 1);
    uint32_t ver = kVersion;
    safeWrite(&ver, 4, 1);
    int64_t ts = meta.timestamp > 0 ? meta.timestamp : static_cast<int64_t>(time(nullptr));
    safeWrite(&ts, 8, 1);
    safeWrite(&meta.lastEventId, 8, 1);

    // Metadata KV pairs
    uint32_t metaCount = static_cast<uint32_t>(meta.extra.size());
    safeWrite(&metaCount, 4, 1);
    for (const auto& [key, value] : meta.extra) {
        if (!ok) break;
        if (!writeString(f, key) || !writeString(f, value)) ok = false;
    }

    // Page table header
    uint32_t pageSize = SearchEngine::kRecordsPerPage;
    safeWrite(&pageSize, 4, 1);
    safeWrite(&totalRecords, 4, 1);
    uint32_t pageCount = static_cast<uint32_t>(pageEntries_.size());
    safeWrite(&pageCount, 4, 1);

    // Page entries
    for (const auto& pe : pageEntries_) {
        safeWrite(&pe.offset, 8, 1);
        safeWrite(&pe.byteLength, 4, 1);
        safeWrite(&pe.recordCount, 2, 1);
        safeWrite(&pe.crc32, 4, 1);
    }

    if (!ok) {
        fclose(f);
        remove(tmpPath.c_str());
        return false;
    }

    fsync(fileno(f));
    fclose(f);

    if (rename(tmpPath.c_str(), ptablePath_.c_str()) != 0) {
        remove(tmpPath.c_str());
        return false;
    }
    return true;
}

// ─── load ───

bool PagedIndexWriter::load(SearchEngine& engine, IndexMetadata* outMeta) {
    FILE* ptf = fopen(ptablePath_.c_str(), "rb");
    if (!ptf) return false;

    // Read ptable header
    uint32_t magic;
    if (fread(&magic, 4, 1, ptf) != 1 || magic != kPtableMagic) { fclose(ptf); return false; }
    uint32_t ver;
    if (fread(&ver, 4, 1, ptf) != 1 || ver != kVersion) { fclose(ptf); return false; }

    IndexMetadata meta;
    meta.formatVersion = ver;
    if (fread(&meta.timestamp, 8, 1, ptf) != 1) { fclose(ptf); return false; }
    if (fread(&meta.lastEventId, 8, 1, ptf) != 1) { fclose(ptf); return false; }

    uint32_t metaCount;
    if (fread(&metaCount, 4, 1, ptf) != 1) { fclose(ptf); return false; }
    if (metaCount > 1000) { fclose(ptf); return false; }
    for (uint32_t i = 0; i < metaCount; i++) {
        std::string key, value;
        // Use file-based readString
        uint32_t klen;
        if (fread(&klen, 4, 1, ptf) != 1 || klen > 65536) { fclose(ptf); return false; }
        key.resize(klen);
        if (klen > 0 && fread(key.data(), 1, klen, ptf) != klen) { fclose(ptf); return false; }
        uint32_t vlen;
        if (fread(&vlen, 4, 1, ptf) != 1 || vlen > 65536) { fclose(ptf); return false; }
        value.resize(vlen);
        if (vlen > 0 && fread(value.data(), 1, vlen, ptf) != vlen) { fclose(ptf); return false; }
        meta.extra[key] = value;
    }

    uint32_t pageSize, totalRecords, pageCount;
    if (fread(&pageSize, 4, 1, ptf) != 1) { fclose(ptf); return false; }
    if (fread(&totalRecords, 4, 1, ptf) != 1) { fclose(ptf); return false; }
    if (fread(&pageCount, 4, 1, ptf) != 1) { fclose(ptf); return false; }

    if (totalRecords > 50'000'000 || pageCount > 100'000) { fclose(ptf); return false; }

    std::vector<PageEntry> entries(pageCount);
    for (uint32_t i = 0; i < pageCount; i++) {
        auto& pe = entries[i];
        if (fread(&pe.offset, 8, 1, ptf) != 1) { fclose(ptf); return false; }
        if (fread(&pe.byteLength, 4, 1, ptf) != 1) { fclose(ptf); return false; }
        if (fread(&pe.recordCount, 2, 1, ptf) != 1) { fclose(ptf); return false; }
        if (fread(&pe.crc32, 4, 1, ptf) != 1) { fclose(ptf); return false; }
    }
    fclose(ptf);

    // Read pages file
    FILE* pf = fopen(pagesPath_.c_str(), "rb");
    if (!pf) return false;

    // Verify pages magic
    uint32_t pagesMagic;
    if (fread(&pagesMagic, 4, 1, pf) != 1 || pagesMagic != kPagesMagic) {
        fclose(pf);
        return false;
    }

    // Get file size
    fseek(pf, 0, SEEK_END);
    uint64_t fileSize = static_cast<uint64_t>(ftell(pf));

    // Read all page blobs and deserialize
    std::vector<FileRecord> allRecords;
    allRecords.reserve(totalRecords);

    for (uint32_t i = 0; i < pageCount; i++) {
        const auto& pe = entries[i];
        if (pe.offset + pe.byteLength > fileSize) {
            fclose(pf);
            return false;
        }

        std::vector<uint8_t> buf(pe.byteLength);
        fseek(pf, static_cast<long>(pe.offset), SEEK_SET);
        if (fread(buf.data(), 1, pe.byteLength, pf) != pe.byteLength) {
            fclose(pf);
            return false;
        }

        // CRC32 verification
        uint32_t crc = IndexWAL::crc32(buf.data(), buf.size());
        if (crc != pe.crc32) {
            std::cerr << "[PagedIndexWriter] CRC mismatch on page " << i
                      << " (expected " << pe.crc32 << ", got " << crc << ")\n";
            fclose(pf);
            return false;
        }

        std::vector<FileRecord> pageRecords;
        if (!deserializePage(buf.data(), buf.size(), pe.recordCount, pageRecords)) {
            fclose(pf);
            return false;
        }
        allRecords.insert(allRecords.end(),
                          std::make_move_iterator(pageRecords.begin()),
                          std::make_move_iterator(pageRecords.end()));
    }
    fclose(pf);

    pageEntries_ = std::move(entries);
    pagesFileSize_ = fileSize;

    if (outMeta) *outMeta = std::move(meta);
    engine.loadRecords(std::move(allRecords));
    return true;
}

// ─── flushDirtyPages ───

bool PagedIndexWriter::flushDirtyPages(SearchEngine& engine, const IndexMetadata& meta) {
    auto dirtyPages = engine.getDirtyPageNumbers();
    if (dirtyPages.empty() && !engine.needsFullRewrite()) {
        return true; // nothing to do
    }

    if (engine.needsFullRewrite()) {
        return fullRewrite(engine, meta);
    }

    uint32_t totalRecords = engine.recordCount();
    uint32_t pageCount = (totalRecords + SearchEngine::kRecordsPerPage - 1) / SearchEngine::kRecordsPerPage;

    // Ensure pageEntries_ is sized correctly
    if (pageEntries_.size() < pageCount) {
        pageEntries_.resize(pageCount, PageEntry{0, 0, 0, 0});
    }

    // Open pages file for appending (create if not exists)
    FILE* pf = fopen(pagesPath_.c_str(), "r+b");
    bool newFile = false;
    if (!pf) {
        pf = fopen(pagesPath_.c_str(), "wb");
        newFile = true;
    }
    if (!pf) return false;

    fcntl(fileno(pf), F_NOCACHE, 1);

    if (newFile) {
        // Write pages magic
        uint32_t magic = kPagesMagic;
        if (fwrite(&magic, 4, 1, pf) != 1) { fclose(pf); return false; }
        pagesFileSize_ = 4;
    } else {
        // Seek to end for appending
        fseek(pf, 0, SEEK_END);
        pagesFileSize_ = static_cast<uint64_t>(ftell(pf));
        // If file is empty or just created, write magic
        if (pagesFileSize_ == 0) {
            uint32_t magic = kPagesMagic;
            if (fwrite(&magic, 4, 1, pf) != 1) { fclose(pf); return false; }
            pagesFileSize_ = 4;
        }
    }

    // Serialize and append each dirty page
    for (uint32_t pageNum : dirtyPages) {
        if (pageNum >= pageCount) continue;

        uint32_t startIdx = pageNum * SearchEngine::kRecordsPerPage;
        uint32_t count = std::min(SearchEngine::kRecordsPerPage,
                                  totalRecords - startIdx);

        // Serialize page records (including tombstones to preserve index alignment)
        std::vector<uint8_t> buf;
        buf.reserve(count * 128);

        engine.forEachRecordInRange(startIdx, count,
            [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                appendRecord(buf, r, path);
            });

        uint32_t crc = IndexWAL::crc32(buf.data(), buf.size());

        // Append to pages file
        uint64_t offset = pagesFileSize_;
        if (fwrite(buf.data(), 1, buf.size(), pf) != buf.size()) {
            fclose(pf);
            return false;
        }
        pagesFileSize_ += buf.size();

        // Update page entry
        auto& pe = pageEntries_[pageNum];
        pe.offset = offset;
        pe.byteLength = static_cast<uint32_t>(buf.size());
        pe.recordCount = static_cast<uint16_t>(count);
        pe.crc32 = crc;
    }

    fsync(fileno(pf));
    fclose(pf);

    // Atomic ptable write
    if (!writePtable(meta, totalRecords)) {
        return false;
    }

    engine.clearDirtyPages();
    return true;
}

// ─── fullRewrite ───

bool PagedIndexWriter::fullRewrite(SearchEngine& engine, const IndexMetadata& meta) {
    uint32_t totalRecords = engine.recordCount();
    uint32_t pageCount = (totalRecords + SearchEngine::kRecordsPerPage - 1) / SearchEngine::kRecordsPerPage;
    if (totalRecords == 0) pageCount = 0;

    std::string tmpPages = pagesPath_ + ".tmp";
    FILE* pf = fopen(tmpPages.c_str(), "wb");
    if (!pf) return false;

    fcntl(fileno(pf), F_NOCACHE, 1);

    // Write pages magic
    uint32_t magic = kPagesMagic;
    if (fwrite(&magic, 4, 1, pf) != 1) { fclose(pf); remove(tmpPages.c_str()); return false; }
    uint64_t writePos = 4;

    std::vector<PageEntry> newEntries;
    newEntries.reserve(pageCount);

    for (uint32_t page = 0; page < pageCount; page++) {
        uint32_t startIdx = page * SearchEngine::kRecordsPerPage;
        uint32_t count = std::min(SearchEngine::kRecordsPerPage,
                                  totalRecords - startIdx);

        std::vector<uint8_t> buf;
        buf.reserve(count * 128);

        engine.forEachRecordInRange(startIdx, count,
            [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                appendRecord(buf, r, path);
            });

        uint32_t crc = IndexWAL::crc32(buf.data(), buf.size());

        if (fwrite(buf.data(), 1, buf.size(), pf) != buf.size()) {
            fclose(pf);
            remove(tmpPages.c_str());
            return false;
        }

        PageEntry pe;
        pe.offset = writePos;
        pe.byteLength = static_cast<uint32_t>(buf.size());
        pe.recordCount = static_cast<uint16_t>(count);
        pe.crc32 = crc;
        newEntries.push_back(pe);

        writePos += buf.size();
    }

    fsync(fileno(pf));
    fclose(pf);

    // Update in-memory state before writing ptable
    pageEntries_ = std::move(newEntries);
    pagesFileSize_ = writePos;

    // Write ptable atomically
    if (!writePtable(meta, totalRecords)) {
        remove(tmpPages.c_str());
        return false;
    }

    // Rename pages file
    if (rename(tmpPages.c_str(), pagesPath_.c_str()) != 0) {
        remove(tmpPages.c_str());
        return false;
    }

    engine.clearDirtyPages();
    engine.clearFullRewriteNeeded();
    return true;
}

// ─── deadSpaceRatio ───

double PagedIndexWriter::deadSpaceRatio() const {
    if (pagesFileSize_ <= 4) return 0.0; // just the magic header
    uint64_t liveBytes = 0;
    for (const auto& pe : pageEntries_) {
        liveBytes += pe.byteLength;
    }
    uint64_t dataSize = pagesFileSize_ - 4; // exclude magic header
    if (dataSize == 0) return 0.0;
    return 1.0 - static_cast<double>(liveBytes) / static_cast<double>(dataSize);
}
