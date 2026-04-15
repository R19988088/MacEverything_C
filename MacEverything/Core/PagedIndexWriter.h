#pragma once
#include "SearchEngine.h"
#include "IndexWAL.h"  // for IndexWAL::crc32()
#include <string>
#include <vector>
#include <cstdint>

/// Page-based incremental persistence for SearchEngine records.
///
/// Two-file format:
///   index.pages  — append-only page blobs (magic "MEPG")
///   index.ptable — atomic page table with offsets/lengths/CRC (magic "MEPT")
///
/// Only dirty pages are appended on flush; old page data becomes dead space.
/// fullRewrite() reclaims dead space by rewriting both files.
class PagedIndexWriter {
public:
    PagedIndexWriter(const std::string& pagesPath, const std::string& ptablePath);

    /// Load records from .ptable + .pages into a SearchEngine.
    /// Returns false if files don't exist or are corrupt.
    bool load(SearchEngine& engine, IndexMetadata* outMeta);

    /// Incrementally write only dirty pages. Appends to .pages, atomically rewrites .ptable.
    /// Clears engine's dirty page bitmap on success.
    bool flushDirtyPages(SearchEngine& engine, const IndexMetadata& meta);

    /// Full rewrite of both files (after compactRecords or dead space reclaim).
    /// Clears engine's dirty pages and fullRewriteNeeded flags on success.
    bool fullRewrite(SearchEngine& engine, const IndexMetadata& meta);

    /// Fraction of .pages file that is dead (superseded) data. Range [0, 1].
    double deadSpaceRatio() const;

    /// Whether the paged files exist and are loadable.
    bool exists() const;

    static constexpr uint32_t kPagesMagic  = 0x4D455047; // "MEPG"
    static constexpr uint32_t kPtableMagic = 0x4D455054; // "MEPT"
    static constexpr uint32_t kVersion     = 1;

private:
    struct PageEntry {
        uint64_t offset;       // byte offset in .pages file
        uint32_t byteLength;   // page blob size in bytes
        uint16_t recordCount;  // actual records in this page (last page may be < kRecordsPerPage)
        uint32_t crc32;        // CRC32 of page blob data
    };

    std::string pagesPath_;
    std::string ptablePath_;
    std::vector<PageEntry> pageEntries_;
    uint64_t pagesFileSize_ = 0;

    /// Write the .ptable file atomically (tmp + fsync + rename).
    bool writePtable(const IndexMetadata& meta, uint32_t totalRecords) const;

    /// Parse a page blob back into FileRecord vector.
    static bool deserializePage(const uint8_t* data, size_t len,
                                uint16_t expectedCount,
                                std::vector<FileRecord>& out);
};
