#include "FlatIndexWriter.h"
#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

FlatIndexWriter::FlatIndexWriter(const std::string& path) : path_(path) {}

bool FlatIndexWriter::exists() const {
    struct stat st;
    return stat(path_.c_str(), &st) == 0 && st.st_size > 0;
}

// ---------------------------------------------------------------------------
// StringPool section helpers
// ---------------------------------------------------------------------------

bool FlatIndexWriter::writeStringPoolSection(FILE* f, const StringPool& pool,
                                              uint32_t& outSize, uint32_t& outCRC) {
    long startPos = ftell(f);

    uint32_t bufSize = static_cast<uint32_t>(pool.rawSize());
    if (fwrite(&bufSize, sizeof(uint32_t), 1, f) != 1) return false;
    if (bufSize > 0 && fwrite(pool.rawBuffer(), 1, bufSize, f) != bufSize) return false;

    uint32_t entryCount = pool.entryCount();
    if (fwrite(&entryCount, sizeof(uint32_t), 1, f) != 1) return false;
    if (entryCount > 0) {
        size_t entryBytes = entryCount * sizeof(StringPool::Entry);
        if (fwrite(pool.entries(), 1, entryBytes, f) != entryBytes) return false;
    }

    long endPos = ftell(f);
    outSize = static_cast<uint32_t>(endPos - startPos);

    // Compute CRC over what we just wrote
    std::vector<uint8_t> buf(outSize);
    fseek(f, startPos, SEEK_SET);
    if (fread(buf.data(), 1, outSize, f) != outSize) return false;
    outCRC = IndexWAL::crc32(buf.data(), outSize);
    fseek(f, endPos, SEEK_SET);

    return true;
}

bool FlatIndexWriter::readStringPoolSection(const uint8_t* data, size_t len, StringPool& pool) {
    if (len < 8) return false;  // minimum: bufSize(4) + entryCount(4)

    size_t pos = 0;
    uint32_t bufSize;
    memcpy(&bufSize, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + bufSize > len) return false;
    const char* buffer = reinterpret_cast<const char*>(data + pos);
    pos += bufSize;

    if (pos + sizeof(uint32_t) > len) return false;
    uint32_t entryCount;
    memcpy(&entryCount, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    size_t entryBytes = entryCount * sizeof(StringPool::Entry);
    if (pos + entryBytes > len) return false;
    const StringPool::Entry* entries = reinterpret_cast<const StringPool::Entry*>(data + pos);

    pool.loadRaw(buffer, bufSize, entries, entryCount);
    return true;
}

// ---------------------------------------------------------------------------
// Array section helpers
// ---------------------------------------------------------------------------

template<typename T>
bool FlatIndexWriter::writeArraySection(FILE* f, const std::vector<T>& vec,
                                         uint32_t& outSize, uint32_t& outCRC) {
    size_t bytes = vec.size() * sizeof(T);
    outSize = static_cast<uint32_t>(bytes);
    if (bytes > 0 && fwrite(vec.data(), 1, bytes, f) != bytes) return false;
    outCRC = (bytes > 0)
        ? IndexWAL::crc32(reinterpret_cast<const uint8_t*>(vec.data()), bytes)
        : IndexWAL::crc32(nullptr, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Metadata section helpers
// ---------------------------------------------------------------------------

bool FlatIndexWriter::writeMetadataSection(FILE* f, const IndexMetadata& meta,
                                            uint32_t& outSize, uint32_t& outCRC) {
    long startPos = ftell(f);

    uint32_t count = static_cast<uint32_t>(meta.extra.size());
    if (fwrite(&count, sizeof(uint32_t), 1, f) != 1) return false;

    for (const auto& [key, value] : meta.extra) {
        uint32_t kLen = static_cast<uint32_t>(key.size());
        if (fwrite(&kLen, sizeof(uint32_t), 1, f) != 1) return false;
        if (kLen > 0 && fwrite(key.data(), 1, kLen, f) != kLen) return false;

        uint32_t vLen = static_cast<uint32_t>(value.size());
        if (fwrite(&vLen, sizeof(uint32_t), 1, f) != 1) return false;
        if (vLen > 0 && fwrite(value.data(), 1, vLen, f) != vLen) return false;
    }

    long endPos = ftell(f);
    outSize = static_cast<uint32_t>(endPos - startPos);

    std::vector<uint8_t> buf(outSize);
    fseek(f, startPos, SEEK_SET);
    if (fread(buf.data(), 1, outSize, f) != outSize) return false;
    outCRC = IndexWAL::crc32(buf.data(), outSize);
    fseek(f, endPos, SEEK_SET);

    return true;
}

bool FlatIndexWriter::readMetadataSection(const uint8_t* data, size_t len, IndexMetadata& meta) {
    if (len < sizeof(uint32_t)) return false;
    size_t pos = 0;

    uint32_t count;
    memcpy(&count, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    for (uint32_t i = 0; i < count; i++) {
        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t kLen;
        memcpy(&kLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + kLen > len) return false;
        std::string key(reinterpret_cast<const char*>(data + pos), kLen);
        pos += kLen;

        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t vLen;
        memcpy(&vLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + vLen > len) return false;
        std::string value(reinterpret_cast<const char*>(data + pos), vLen);
        pos += vLen;

        meta.extra[std::move(key)] = std::move(value);
    }
    return true;
}

// ---------------------------------------------------------------------------
// fullRewrite — serialize all SoA columns to v6 file
// ---------------------------------------------------------------------------

bool FlatIndexWriter::fullRewrite(SearchEngine& engine, const IndexMetadata& meta) {
    auto snap = engine.snapshotForV6();

    std::string tmpPath = path_ + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "w+b");
    if (!f) {
        LOG_ERROR("FlatIndexWriter", "Cannot open " << tmpPath << " for writing");
        return false;
    }

    // Bypass page cache to avoid evicting hot search data
    fcntl(fileno(f), F_NOCACHE, 1);

    // Write placeholder header + section table (will be filled later)
    Header header{};
    memcpy(header.magic, kMagic, 4);
    header.version = kVersion;
    header.recordCount = snap.origNamePool.entryCount();
    header.liveCount = snap.liveCount;
    header.timestamp = meta.timestamp > 0 ? meta.timestamp : static_cast<int64_t>(time(nullptr));
    header.lastEventId = meta.lastEventId;
    header.sectionCount = kSectionCount;

    if (fwrite(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Reserve space for section table
    SectionEntry sections[kSectionCount] = {};
    long sectionTablePos = ftell(f);
    if (fwrite(sections, sizeof(SectionEntry), kSectionCount, f) != kSectionCount) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Helper to fill a section entry
    auto fillSection = [&](uint32_t idx, uint32_t sectionId, uint64_t offset,
                           uint32_t byteLen, uint32_t crc) {
        sections[idx].sectionId = sectionId;
        sections[idx].reserved = 0;
        sections[idx].offset = offset;
        sections[idx].byteLength = byteLen;
        sections[idx].crc32 = crc;
    };

    bool ok = true;

    // Section 1: NAMES_ORIG
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.origNamePool, size, crc);
        fillSection(0, kSectionNamesOrig, offset, size, crc);
    }

    // Section 2: NAMES_LOWER
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.namePool, size, crc);
        fillSection(1, kSectionNamesLower, offset, size, crc);
    }

    // Section 3: PATH_POOL
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.pathPool, size, crc);
        fillSection(2, kSectionPathPool, offset, size, crc);
    }

    // Section 4: LOWER_PATH_POOL
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.lowerPathPool, size, crc);
        fillSection(3, kSectionLowerPathPool, offset, size, crc);
    }

    // Section 5: PATH_INDICES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.pathIndices, size, crc);
        fillSection(4, kSectionPathIndices, offset, size, crc);
    }

    // Section 6: TYPES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.types, size, crc);
        fillSection(5, kSectionTypes, offset, size, crc);
    }

    // Section 7: SIZES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.sizes, size, crc);
        fillSection(6, kSectionSizes, offset, size, crc);
    }

    // Section 8: MOD_TIMES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.modTimes, size, crc);
        fillSection(7, kSectionModTimes, offset, size, crc);
    }

    // Section 9: INODES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.inodes, size, crc);
        fillSection(8, kSectionInodes, offset, size, crc);
    }

    // Section 10: DEV_IDS
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.devIds, size, crc);
        fillSection(9, kSectionDevIds, offset, size, crc);
    }

    // Section 11: METADATA_KV
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeMetadataSection(f, meta, size, crc);
        fillSection(10, kSectionMetadataKV, offset, size, crc);
    }

    if (!ok) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Write section table back
    fseek(f, sectionTablePos, SEEK_SET);
    if (fwrite(sections, sizeof(SectionEntry), kSectionCount, f) != kSectionCount) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Compute and write header CRC (over first 60 bytes, excluding headerCRC field)
    fseek(f, 0, SEEK_SET);
    uint8_t headerBytes[kHeaderSize];
    if (fread(headerBytes, 1, kHeaderSize, f) != kHeaderSize) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }
    // headerCRC is at offset 44 (after magic(4)+version(4)+recordCount(4)+liveCount(4)
    //   +timestamp(8)+lastEventId(8)+sectionCount(4) = 36, then headerCRC at 40)
    // Actually: 4+4+4+4+8+8+4 = 36, headerCRC at offset 36, reserved at 40
    uint32_t hdrCRC = IndexWAL::crc32(headerBytes, 36);
    header.headerCRC = hdrCRC;
    fseek(f, 0, SEEK_SET);
    if (fwrite(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    fsync(fileno(f));
    fclose(f);

    if (rename(tmpPath.c_str(), path_.c_str()) != 0) {
        remove(tmpPath.c_str());
        return false;
    }

    LOG_INFO("FlatIndexWriter", "v6 written: " << header.recordCount << " records ("
             << header.liveCount << " live), " << kSectionCount << " sections");
    return true;
}

// ---------------------------------------------------------------------------
// load — deserialize v6 file into SearchEngine via loadRecordsV6()
// ---------------------------------------------------------------------------

bool FlatIndexWriter::load(SearchEngine& engine, IndexMetadata* outMeta) {
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;

    // Read header
    Header header{};
    if (fread(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); return false;
    }

    // Verify magic
    if (memcmp(header.magic, kMagic, 4) != 0) {
        fclose(f); return false;
    }

    // Verify version
    if (header.version != kVersion) {
        LOG_ERROR("FlatIndexWriter", "Unknown v6 version: " << header.version);
        fclose(f); return false;
    }

    // Verify header CRC
    {
        uint8_t headerBytes[kHeaderSize];
        fseek(f, 0, SEEK_SET);
        if (fread(headerBytes, 1, kHeaderSize, f) != kHeaderSize) {
            fclose(f); return false;
        }
        uint32_t expectedCRC = IndexWAL::crc32(headerBytes, 36);
        if (header.headerCRC != expectedCRC) {
            LOG_ERROR("FlatIndexWriter", "Header CRC mismatch");
            fclose(f); return false;
        }
    }

    // Read section table
    fseek(f, kHeaderSize, SEEK_SET);
    std::vector<SectionEntry> sections(header.sectionCount);
    if (fread(sections.data(), sizeof(SectionEntry), header.sectionCount, f)
        != header.sectionCount) {
        fclose(f); return false;
    }

    // Read all section data into memory
    // Build a map from sectionId -> (data pointer, length)
    struct SectionData {
        std::vector<uint8_t> data;
        uint32_t expectedCRC;
    };
    std::unordered_map<uint32_t, SectionData> sectionMap;
    sectionMap.reserve(header.sectionCount);

    for (const auto& entry : sections) {
        SectionData sd;
        sd.data.resize(entry.byteLength);
        sd.expectedCRC = entry.crc32;
        fseek(f, static_cast<long>(entry.offset), SEEK_SET);
        if (fread(sd.data.data(), 1, entry.byteLength, f) != entry.byteLength) {
            LOG_ERROR("FlatIndexWriter", "Failed to read section " << entry.sectionId);
            fclose(f); return false;
        }

        // Verify CRC
        uint32_t actualCRC = IndexWAL::crc32(sd.data.data(), sd.data.size());
        if (actualCRC != sd.expectedCRC) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << entry.sectionId);
            fclose(f); return false;
        }

        sectionMap[entry.sectionId] = std::move(sd);
    }
    fclose(f);

    // Deserialize each section
    auto getSection = [&](uint32_t id) -> const SectionData* {
        auto it = sectionMap.find(id);
        return (it != sectionMap.end()) ? &it->second : nullptr;
    };

    // Required sections
    StringPool origNamePool, namePool, pathPool, lowerPathPool;

    auto* s1 = getSection(kSectionNamesOrig);
    if (!s1 || !readStringPoolSection(s1->data.data(), s1->data.size(), origNamePool)) {
        LOG_ERROR("FlatIndexWriter", "Failed to read NAMES_ORIG section"); return false;
    }

    auto* s2 = getSection(kSectionNamesLower);
    if (!s2 || !readStringPoolSection(s2->data.data(), s2->data.size(), namePool)) {
        LOG_ERROR("FlatIndexWriter", "Failed to read NAMES_LOWER section"); return false;
    }

    auto* s3 = getSection(kSectionPathPool);
    if (!s3 || !readStringPoolSection(s3->data.data(), s3->data.size(), pathPool)) {
        LOG_ERROR("FlatIndexWriter", "Failed to read PATH_POOL section"); return false;
    }

    auto* s4 = getSection(kSectionLowerPathPool);
    if (!s4 || !readStringPoolSection(s4->data.data(), s4->data.size(), lowerPathPool)) {
        LOG_ERROR("FlatIndexWriter", "Failed to read LOWER_PATH_POOL section"); return false;
    }

    uint32_t n = origNamePool.entryCount();

    // Array sections — direct memcpy into vectors
    auto readArray = [&]<typename T>(uint32_t sectionId, std::vector<T>& vec, const char* name) -> bool {
        auto* s = getSection(sectionId);
        if (!s) {
            LOG_ERROR("FlatIndexWriter", "Missing section: " << name);
            return false;
        }
        size_t expectedBytes = n * sizeof(T);
        if (s->data.size() != expectedBytes) {
            LOG_ERROR("FlatIndexWriter", "Section " << name << " size mismatch: "
                      << s->data.size() << " vs expected " << expectedBytes);
            return false;
        }
        vec.resize(n);
        memcpy(vec.data(), s->data.data(), expectedBytes);
        return true;
    };

    std::vector<uint32_t> pathIndices;
    std::vector<uint8_t> types;
    std::vector<uint64_t> sizes;
    std::vector<int64_t> modTimes;
    std::vector<uint64_t> inodes;
    std::vector<int32_t> devIds;

    if (!readArray(kSectionPathIndices, pathIndices, "PATH_INDICES")) return false;
    if (!readArray(kSectionTypes, types, "TYPES")) return false;
    if (!readArray(kSectionSizes, sizes, "SIZES")) return false;
    if (!readArray(kSectionModTimes, modTimes, "MOD_TIMES")) return false;
    if (!readArray(kSectionInodes, inodes, "INODES")) return false;
    if (!readArray(kSectionDevIds, devIds, "DEV_IDS")) return false;

    // Metadata (optional)
    IndexMetadata meta;
    meta.formatVersion = 6;
    meta.timestamp = header.timestamp;
    meta.lastEventId = header.lastEventId;

    auto* s11 = getSection(kSectionMetadataKV);
    if (s11) {
        readMetadataSection(s11->data.data(), s11->data.size(), meta);
    }

    if (outMeta) *outMeta = std::move(meta);

    // Install into engine
    engine.loadRecordsV6(
        std::move(origNamePool),
        std::move(namePool),
        std::move(pathIndices),
        std::move(pathPool),
        std::move(lowerPathPool),
        std::move(types),
        std::move(sizes),
        std::move(modTimes),
        std::move(inodes),
        std::move(devIds)
    );

    LOG_INFO("FlatIndexWriter", "v6 loaded: " << n << " records, lastEventId="
             << header.lastEventId);
    return true;
}
