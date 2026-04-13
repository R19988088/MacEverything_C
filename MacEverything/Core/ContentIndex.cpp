#include "ContentIndex.h"
#include <algorithm>
#include <fstream>
#include <cctype>
#include <cstring>
#include <atomic>
#include <thread>
#include <dispatch/dispatch.h>

// --- Magic and version for binary persistence ---
static constexpr char CONTENT_MAGIC[4] = {'M', 'E', 'C', 'I'};
static constexpr uint32_t CONTENT_FORMAT_VERSION = 1;

ContentIndex::ContentIndex() {
    // No default extensions — content indexing is opt-in.
    // Users must configure extensions via Content Settings.
}

// --- Configuration ---

void ContentIndex::setExtensions(const std::vector<std::string>& exts) {
    std::unique_lock lock(mutex_);
    extensions_.clear();
    for (const auto& ext : exts) {
        extensions_.insert(toLower(ext));
    }
}

void ContentIndex::setMaxFileSize(uint64_t bytes) {
    std::unique_lock lock(mutex_);
    maxFileSize_ = bytes;
}

std::vector<std::string> ContentIndex::getExtensions() const {
    std::shared_lock lock(mutex_);
    return std::vector<std::string>(extensions_.begin(), extensions_.end());
}

uint64_t ContentIndex::getMaxFileSize() const {
    std::shared_lock lock(mutex_);
    return maxFileSize_;
}

// --- Helpers ---

std::string ContentIndex::toLower(const std::string& s) {
    std::string result;
    result.resize(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return result;
}

uint64_t ContentIndex::hashContent(const std::string& content) {
    // FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : content) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool ContentIndex::isBinaryFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return true; // can't read = skip

    char buf[8192];
    size_t bytesRead = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i < bytesRead; i++) {
        if (buf[i] == '\0') return true;
    }
    return false;
}

bool ContentIndex::hasAllowedExtension(const std::string& filename) const {
    std::shared_lock lock(mutex_);
    return hasAllowedExtensionLocked(filename);
}

bool ContentIndex::hasAllowedExtensionLocked(const std::string& filename) const {
    // Find last dot
    size_t dotPos = filename.rfind('.');
    if (dotPos == std::string::npos || dotPos == filename.size() - 1) {
        // No extension — check if "makefile" etc. is in extensions
        std::string lowerName = toLower(filename);
        return extensions_.count(lowerName) > 0;
    }

    std::string ext = toLower(filename.substr(dotPos + 1));
    return extensions_.count(ext) > 0;
}

std::string ContentIndex::readFileContent(const std::string& path, uint64_t maxSize) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};

    // Get file size
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    if (fileSize <= 0 || static_cast<uint64_t>(fileSize) > maxSize) {
        fclose(f);
        return {};
    }

    fseek(f, 0, SEEK_SET);
    std::string content;
    content.resize(static_cast<size_t>(fileSize));
    size_t bytesRead = fread(content.data(), 1, content.size(), f);
    fclose(f);

    if (bytesRead != content.size()) {
        content.resize(bytesRead);
    }
    return content;
}

// --- Trigram extraction ---

std::vector<Trigram> ContentIndex::extractTrigrams(const std::string& text) {
    if (text.size() < 3) return {};

    // Bitmap dedup: 2^24 = 16M bits = 2MB, much faster than unordered_set for large texts
    static constexpr size_t kBitmapSize = 1 << 24;
    std::vector<bool> seen(kBitmapSize, false);
    std::vector<Trigram> result;

    for (size_t i = 0; i + 2 < text.size(); i++) {
        uint8_t a = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(text[i])));
        uint8_t b = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(text[i + 1])));
        uint8_t c = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(text[i + 2])));
        Trigram t = makeTrigram(a, b, c);
        if (!seen[t]) {
            seen[t] = true;
            result.push_back(t);
        }
    }

    return result;
}

// --- Snippet generation ---

std::string ContentIndex::generateSnippet(const std::string& path,
                                           const std::string& keyword,
                                           uint32_t& outOffset,
                                           uint32_t contextChars) {
    outOffset = 0;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};

    // Read up to 1MB for snippet search
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) { fclose(f); return {}; }

    size_t readSize = std::min(static_cast<size_t>(fileSize), size_t(1024 * 1024));
    std::string content(readSize, '\0');
    size_t bytesRead = fread(content.data(), 1, readSize, f);
    fclose(f);
    content.resize(bytesRead);

    // Case-insensitive search
    std::string lowerContent;
    lowerContent.resize(content.size());
    for (size_t i = 0; i < content.size(); i++) {
        lowerContent[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(content[i])));
    }

    std::string lowerKey;
    lowerKey.resize(keyword.size());
    for (size_t i = 0; i < keyword.size(); i++) {
        lowerKey[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(keyword[i])));
    }

    size_t pos = lowerContent.find(lowerKey);
    if (pos == std::string::npos) return {};

    outOffset = static_cast<uint32_t>(pos);

    // Extract context around the match
    size_t start = (pos > contextChars) ? pos - contextChars : 0;
    size_t end = std::min(pos + keyword.size() + contextChars, content.size());

    // Adjust start to a line boundary or word boundary if possible
    if (start > 0) {
        size_t newlinePos = content.rfind('\n', pos);
        if (newlinePos != std::string::npos && newlinePos >= start) {
            start = newlinePos + 1;
        }
    }

    std::string snippet = content.substr(start, end - start);

    // Replace newlines with spaces for single-line display
    for (char& ch : snippet) {
        if (ch == '\n' || ch == '\r') ch = ' ';
    }

    // Trim leading/trailing whitespace
    size_t firstNonSpace = snippet.find_first_not_of(" \t");
    if (firstNonSpace != std::string::npos) {
        snippet = snippet.substr(firstNonSpace);
    }
    size_t lastNonSpace = snippet.find_last_not_of(" \t");
    if (lastNonSpace != std::string::npos) {
        snippet = snippet.substr(0, lastNonSpace + 1);
    }

    // Add ellipsis indicators
    std::string result;
    if (start > 0) result += "...";
    result += snippet;
    if (end < content.size()) result += "...";

    return result;
}

// --- Indexing ---

bool ContentIndex::indexFile(uint32_t fileIndex, const std::string& fullPath) {
    // Check extension (read lock for config)
    {
        std::shared_lock lock(mutex_);
        // Extract filename from path
        size_t lastSlash = fullPath.rfind('/');
        std::string filename = (lastSlash != std::string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
        if (extensions_.empty() || !hasAllowedExtensionLocked(filename)) return false;
    }

    // Single I/O: read content and check for binary in one pass
    uint64_t maxSize;
    {
        std::shared_lock lock(mutex_);
        maxSize = maxFileSize_;
    }

    std::string content = readFileContent(fullPath, maxSize);
    if (content.empty()) return false;

    // Check for NUL bytes (binary detection) in the content we already read
    size_t checkLen = std::min(content.size(), size_t(8192));
    for (size_t i = 0; i < checkLen; i++) {
        if (content[i] == '\0') return false;
    }

    uint64_t hash = hashContent(content);
    auto trigrams = extractTrigrams(content);

    // Check if already indexed with same hash
    {
        std::shared_lock lock(mutex_);
        auto it = fileInfos_.find(fileIndex);
        if (it != fileInfos_.end() && it->second.contentHash == hash) {
            return true; // already up-to-date
        }
    }

    // Update index (exclusive lock)
    std::unique_lock lock(mutex_);

    // Remove old entry if exists
    auto oldIt = fileInfos_.find(fileIndex);
    if (oldIt != fileInfos_.end()) {
        // Remove from inverted index
        for (Trigram tri : oldIt->second.trigrams) {
            auto& postingList = invertedIndex_[tri];
            postingList.erase(
                std::remove(postingList.begin(), postingList.end(), fileIndex),
                postingList.end()
            );
            if (postingList.empty()) {
                invertedIndex_.erase(tri);
            }
        }
    }

    // Add to inverted index (sorted insertion)
    for (Trigram tri : trigrams) {
        auto& list = invertedIndex_[tri];
        auto pos = std::lower_bound(list.begin(), list.end(), fileIndex);
        if (pos == list.end() || *pos != fileIndex) {
            list.insert(pos, fileIndex);
        }
    }

    // Store file info
    ContentFileInfo info;
    info.contentHash = hash;
    info.trigrams = std::move(trigrams);
    fileInfos_[fileIndex] = std::move(info);

    return true;
}

void ContentIndex::removeFile(uint32_t fileIndex) {
    std::unique_lock lock(mutex_);
    removeFileInternal(fileIndex);
}

void ContentIndex::removeFileInternal(uint32_t fileIndex) {
    auto it = fileInfos_.find(fileIndex);
    if (it == fileInfos_.end()) return;

    // Remove from inverted index
    for (Trigram tri : it->second.trigrams) {
        auto invIt = invertedIndex_.find(tri);
        if (invIt != invertedIndex_.end()) {
            auto& postingList = invIt->second;
            postingList.erase(
                std::remove(postingList.begin(), postingList.end(), fileIndex),
                postingList.end()
            );
            if (postingList.empty()) {
                invertedIndex_.erase(invIt);
            }
        }
    }

    fileInfos_.erase(it);
}

bool ContentIndex::isFileIndexed(uint32_t fileIndex) const {
    std::shared_lock lock(mutex_);
    return fileInfos_.count(fileIndex) > 0;
}

void ContentIndex::insertFileInfo(uint32_t fileIndex, uint64_t contentHash, std::vector<Trigram>&& trigrams) {
    std::unique_lock lock(mutex_);

    // Remove old if exists
    removeFileInternal(fileIndex);

    // Add to inverted index (sorted insertion)
    for (Trigram tri : trigrams) {
        auto& list = invertedIndex_[tri];
        auto pos = std::lower_bound(list.begin(), list.end(), fileIndex);
        if (pos == list.end() || *pos != fileIndex) {
            list.insert(pos, fileIndex);
        }
    }

    ContentFileInfo info;
    info.contentHash = contentHash;
    info.trigrams = std::move(trigrams);
    fileInfos_[fileIndex] = std::move(info);
}

bool ContentIndex::getFileInfo(uint32_t fileIndex, ContentFileInfo& info) const {
    std::shared_lock lock(mutex_);
    auto it = fileInfos_.find(fileIndex);
    if (it == fileInfos_.end()) return false;
    info = it->second;
    return true;
}

// --- Querying ---

std::vector<ContentMatch> ContentIndex::query(const std::string& keyword, uint32_t maxResults) const {
    if (keyword.empty()) return {};

    std::string lowerKey = toLower(keyword);

    std::shared_lock lock(mutex_);

    std::vector<uint32_t> candidates;

    if (lowerKey.size() >= 3) {
        // Extract trigrams from keyword
        auto keyTrigrams = extractTrigrams(lowerKey);
        if (keyTrigrams.empty()) return {};

        // Find the trigram with the smallest posting list (for efficiency)
        const std::vector<uint32_t>* smallest = nullptr;
        for (Trigram tri : keyTrigrams) {
            auto it = invertedIndex_.find(tri);
            if (it == invertedIndex_.end()) {
                // This trigram doesn't exist in any file — no matches possible
                return {};
            }
            if (!smallest || it->second.size() < smallest->size()) {
                smallest = &it->second;
            }
        }

        if (!smallest) return {};

        // Remember which trigram is the smallest so we can skip it during intersection
        const std::vector<uint32_t>* smallestPtr = smallest;

        // Intersect: start with smallest posting list, check all other trigrams
        for (uint32_t fileIdx : *smallestPtr) {
            bool inAll = true;
            for (Trigram tri : keyTrigrams) {
                auto it = invertedIndex_.find(tri);
                if (it == invertedIndex_.end()) { inAll = false; break; }

                // Skip the posting list we're already iterating
                if (&it->second == smallestPtr) continue;

                // Binary search in sorted posting list
                const auto& list = it->second;
                if (!std::binary_search(list.begin(), list.end(), fileIdx)) {
                    inAll = false;
                    break;
                }
            }
            if (inAll) {
                candidates.push_back(fileIdx);
            }
        }
    } else {
        // Short keyword: collect all indexed file indices for brute-force
        candidates.reserve(fileInfos_.size());
        for (const auto& [fileIdx, _] : fileInfos_) {
            candidates.push_back(fileIdx);
        }
    }

    // Release shared lock before doing file I/O for verification
    lock.unlock();

    // Verify candidates by actually reading files and generate snippets
    // Use dispatch_apply for parallelism
    std::vector<ContentMatch> results;
    std::mutex resultMutex;
    std::atomic<uint32_t> found{0};

    unsigned numThreads = std::min(static_cast<unsigned>(candidates.size()),
                                    std::max(1u, std::thread::hardware_concurrency()));
    if (numThreads > 16) numThreads = 16;

    // We need the SearchEngine to resolve fileIndex → path.
    // Since we can't access SearchEngine from here, the bridge layer will need to
    // provide path resolution. For now, store the candidates and let the bridge
    // layer do verification + snippet generation.
    //
    // Actually, we return candidates as ContentMatch with empty snippets.
    // The bridge layer fills in snippets using SearchEngine::getRecord().

    for (uint32_t fileIdx : candidates) {
        if (maxResults > 0 && found.load(std::memory_order_relaxed) >= maxResults) break;

        ContentMatch match;
        match.fileIndex = fileIdx;
        match.matchOffset = 0;
        // snippet will be filled by the bridge layer which has access to file paths
        results.push_back(std::move(match));
        found.fetch_add(1, std::memory_order_relaxed);
    }

    return results;
}

// --- Stats ---

uint32_t ContentIndex::indexedFileCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(fileInfos_.size());
}

// --- Persistence ---

static bool writeU32(FILE* f, uint32_t v) {
    return fwrite(&v, sizeof(uint32_t), 1, f) == 1;
}

static bool writeU64(FILE* f, uint64_t v) {
    return fwrite(&v, sizeof(uint64_t), 1, f) == 1;
}

static bool readU32(FILE* f, uint32_t& v) {
    return fread(&v, sizeof(uint32_t), 1, f) == 1;
}

static bool readU64(FILE* f, uint64_t& v) {
    return fread(&v, sizeof(uint64_t), 1, f) == 1;
}

bool ContentIndex::saveToFile(const std::string& path) const {
    std::shared_lock lock(mutex_);

    std::string tmpPath = path + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) return false;

    bool ok = true;
    auto safeWrite = [&](const void* ptr, size_t size, size_t count) {
        if (ok && fwrite(ptr, size, count, f) != count) ok = false;
    };

    // Header
    safeWrite(CONTENT_MAGIC, 1, 4);
    uint32_t version = CONTENT_FORMAT_VERSION;
    safeWrite(&version, sizeof(uint32_t), 1);

    // File count
    uint32_t fileCount = static_cast<uint32_t>(fileInfos_.size());
    safeWrite(&fileCount, sizeof(uint32_t), 1);

    // Per-file: fileIndex(4) + contentHash(8) + trigramCount(4) + trigrams(4 each)
    for (const auto& [fileIndex, info] : fileInfos_) {
        if (!ok) break;
        safeWrite(&fileIndex, sizeof(uint32_t), 1);
        safeWrite(&info.contentHash, sizeof(uint64_t), 1);
        uint32_t triCount = static_cast<uint32_t>(info.trigrams.size());
        safeWrite(&triCount, sizeof(uint32_t), 1);
        if (triCount > 0) {
            safeWrite(info.trigrams.data(), sizeof(Trigram), triCount);
        }
    }

    fclose(f);

    if (!ok) {
        remove(tmpPath.c_str());
        return false;
    }

    if (rename(tmpPath.c_str(), path.c_str()) != 0) {
        remove(tmpPath.c_str());
        return false;
    }
    return true;
}

bool ContentIndex::loadFromFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    // Verify magic
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, CONTENT_MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }

    uint32_t version;
    if (!readU32(f, version) || version > CONTENT_FORMAT_VERSION) {
        fclose(f);
        return false;
    }

    uint32_t fileCount;
    if (!readU32(f, fileCount)) {
        fclose(f);
        return false;
    }

    // Read all file entries
    std::unordered_map<uint32_t, ContentFileInfo> newFileInfos;
    std::unordered_map<Trigram, std::vector<uint32_t>> newInvertedIndex;
    newFileInfos.reserve(fileCount);

    for (uint32_t i = 0; i < fileCount; i++) {
        uint32_t fileIndex;
        uint64_t contentHash;
        uint32_t triCount;

        if (!readU32(f, fileIndex) || !readU64(f, contentHash) || !readU32(f, triCount)) {
            fclose(f);
            return false;
        }

        // Sanity limit
        if (triCount > 1000000) {
            fclose(f);
            return false;
        }

        std::vector<Trigram> trigrams(triCount);
        if (triCount > 0 && fread(trigrams.data(), sizeof(Trigram), triCount, f) != triCount) {
            fclose(f);
            return false;
        }

        // Build inverted index (sorted insertion)
        for (Trigram tri : trigrams) {
            auto& list = newInvertedIndex[tri];
            auto pos = std::lower_bound(list.begin(), list.end(), fileIndex);
            if (pos == list.end() || *pos != fileIndex) {
                list.insert(pos, fileIndex);
            }
        }

        ContentFileInfo info;
        info.contentHash = contentHash;
        info.trigrams = std::move(trigrams);
        newFileInfos[fileIndex] = std::move(info);
    }

    fclose(f);

    // Swap into live data
    std::unique_lock lock(mutex_);
    invertedIndex_ = std::move(newInvertedIndex);
    fileInfos_ = std::move(newFileInfos);

    return true;
}
