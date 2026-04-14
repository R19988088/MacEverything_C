#import "MacSearchBridge_Internal.h"
#include <dispatch/dispatch.h>
#include <unistd.h>

@implementation MacSearchBridge (Content)

- (void)startContentIndexing {
    auto engine = [self safeEngine]; // C-4
    if (!engine || !_contentIndex) return;
    _isContentIndexing.store(true, std::memory_order_relaxed);
    _cancelContentIndexing.store(false, std::memory_order_relaxed); // H-8: reset cancel flag

    auto contentIndex = _contentIndex;
    auto contentPersistence = _contentPersistence;
    auto* shuttingDown = &_shuttingDown;
    auto* cancelFlag = &_cancelContentIndexing; // H-8
    __weak MacSearchBridge *weakSelf = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        uint32_t total = engine->recordCount();
        auto indexed = std::make_shared<std::atomic<uint32_t>>(0);
        auto lastReported = std::make_shared<std::atomic<uint32_t>>(0);

        // H3 fix: Use dispatch_apply for parallel content indexing.
        // contentIndex->indexFile() is internally thread-safe (acquires its own lock).
        // contentPersistence->walAppendAdd() is also thread-safe (WAL has its own mutex).
        // This overlaps file I/O across threads for significant speedup.
        dispatch_queue_t concurrentQ = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        dispatch_apply(total, concurrentQ, ^(size_t i) {
            if (shuttingDown->load(std::memory_order_relaxed)) return;
            if (cancelFlag->load(std::memory_order_relaxed)) return;

            auto record = engine->getRecord(static_cast<uint32_t>(i));
            if (record.type != 1) return; // only regular files

            std::string fullPath = SearchEngine::makeFullPath(record.path, record.name);
            bool didIndex = contentIndex->indexFile(static_cast<uint32_t>(i), fullPath);

            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(static_cast<uint32_t>(i), info)) {
                    contentPersistence->walAppendAdd(static_cast<uint32_t>(i), info.contentHash, info.trigrams);
                }
            }

            uint32_t current = indexed->fetch_add(1, std::memory_order_relaxed) + 1;

            // Report progress every 500 files
            if (current - lastReported->load(std::memory_order_relaxed) >= 500) {
                lastReported->store(current, std::memory_order_relaxed);
                uint32_t c = current;
                dispatch_async(dispatch_get_main_queue(), ^{
                    MacSearchBridge *strongSelf = weakSelf;
                    if (strongSelf && strongSelf.onContentIndexProgress) {
                        strongSelf.onContentIndexProgress(c, total);
                    }
                });
            }
        });

        uint32_t totalIndexed = contentIndex->indexedFileCount();

        dispatch_async(dispatch_get_main_queue(), ^{
            MacSearchBridge *strongSelf = weakSelf;
            if (strongSelf) {
                strongSelf->_isContentIndexing.store(false, std::memory_order_relaxed);
                if (strongSelf.onContentIndexComplete) {
                    strongSelf.onContentIndexComplete(totalIndexed);
                }
            }
        });
    });
}

- (void)setupContentPersistence {
    if (!_contentIndex) return;

    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString *cachesDir = [paths firstObject];
    NSString *appCacheDir = [cachesDir stringByAppendingPathComponent:@"com.maceverything.app"];

    // Ensure directory exists
    NSError *dirError = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:appCacheDir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:&dirError]) {
        NSLog(@"[MacSearchBridge] Failed to create content index directory: %@", dirError);
        return;
    }

    std::string basePath = std::string([[appCacheDir stringByAppendingPathComponent:@"content_index.bin"] UTF8String]);
    std::string walPath = std::string([[appCacheDir stringByAppendingPathComponent:@"content_index.wal"] UTF8String]);

    _contentPersistence = std::make_shared<ContentIndexPersistence>(_contentIndex, basePath, walPath);
    _contentPersistence->load();
    _contentPersistence->attachWAL();
    _contentPersistence->startAutoCompaction(300.0);
}

- (NSArray<MEContentResult *> *)queryContent:(NSString *)keyword maxResults:(uint32_t)maxResults {
    auto engine = [self safeEngine]; // C-4
    if (!engine || !_contentIndex) return @[];

    std::string key([keyword UTF8String]);
    if (key.empty()) return @[];

    auto matches = _contentIndex->query(key, maxResults);
    if (matches.empty()) return @[];

    // Pre-resolve file paths (needs engine lock, do it once)
    struct CandidateInfo {
        uint32_t fileIndex;
        std::string name;
        std::string fullPath;
        uint8_t fileType;
    };
    std::vector<CandidateInfo> candidates;
    candidates.reserve(matches.size());
    for (const auto& match : matches) {
        auto record = engine->getRecord(match.fileIndex);
        if (record.type == 0) continue; // skip tombstones
        CandidateInfo info;
        info.fileIndex = match.fileIndex;
        info.name = std::move(record.name);
        info.fullPath = SearchEngine::makeFullPath(record.path, info.name);
        info.fileType = record.type;
        candidates.push_back(std::move(info));
    }

    if (candidates.empty()) return @[];

    // Parallel snippet generation using dispatch_apply
    struct SnippetResult {
        std::string snippet;
        uint32_t offset = 0;
        bool valid = false;
    };
    __block std::vector<SnippetResult> snippetResults(candidates.size());

    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_apply(candidates.size(), queue, ^(size_t i) {
        uint32_t offset = 0;
        std::string snippet = ContentIndex::generateSnippet(candidates[i].fullPath, key, offset);
        if (!snippet.empty()) {
            snippetResults[i].snippet = std::move(snippet);
            snippetResults[i].offset = offset;
            snippetResults[i].valid = true;
        }
    });

    // Collect valid results (single-threaded, no lock needed)
    NSMutableArray<MEContentResult *> *results = [NSMutableArray arrayWithCapacity:candidates.size()];
    for (size_t i = 0; i < candidates.size(); i++) {
        if (!snippetResults[i].valid) continue;
        MEContentResult *result = [[MEContentResult alloc]
            initWithFileName:[NSString stringWithUTF8String:candidates[i].name.c_str()]
                    filePath:[NSString stringWithUTF8String:candidates[i].fullPath.c_str()]
                     snippet:[NSString stringWithUTF8String:snippetResults[i].snippet.c_str()]
                 matchOffset:snippetResults[i].offset
                    fileType:candidates[i].fileType];
        [results addObject:result];
    }

    return results;
}

- (void)setContentExtensions:(NSArray<NSString *> *)extensions {
    if (!_contentIndex) return;
    std::vector<std::string> exts;
    exts.reserve(extensions.count);
    for (NSString *ext in extensions) {
        exts.push_back(std::string([ext UTF8String]));
    }
    _contentIndex->setExtensions(exts);
}

- (void)setContentMaxFileSize:(uint64_t)bytes {
    if (_contentIndex) {
        _contentIndex->setMaxFileSize(bytes);
    }
}

- (uint32_t)contentIndexedFileCount {
    return _contentIndex ? _contentIndex->indexedFileCount() : 0;
}

- (NSArray<NSString *> *)contentGetExtensions {
    if (!_contentIndex) return @[];
    auto exts = _contentIndex->getExtensions();
    NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:exts.size()];
    for (const auto& ext : exts) {
        [result addObject:[NSString stringWithUTF8String:ext.c_str()]];
    }
    return result;
}

- (uint64_t)contentGetMaxFileSize {
    return _contentIndex ? _contentIndex->getMaxFileSize() : (1 * 1024 * 1024);
}

- (void)rebuildContentIndex {
    auto engine = [self safeEngine]; // C-4
    if (!engine || !_contentIndex) return;

    // H-8: Cancel in-flight content indexing and wait for it to stop
    if (_isContentIndexing.load(std::memory_order_relaxed)) {
        _cancelContentIndexing.store(true, std::memory_order_relaxed);
        // Spin briefly to let the indexing loop notice and exit
        for (int i = 0; i < 100 && _isContentIndexing.load(std::memory_order_relaxed); i++) {
            usleep(10000); // 10ms
        }
    }

    // H8 fix: Stop old persistence's auto-compaction timer before replacing,
    // to prevent timer firing into a dangling reference.
    if (_contentPersistence) {
        _contentPersistence->stopAutoCompactionAndWait();
        _contentPersistence.reset();
    }

    // Clear old content index data
    {
        auto exts = _contentIndex->getExtensions();
        auto maxSize = _contentIndex->getMaxFileSize();

        // Re-create content index to clear all data
        _contentIndex = std::make_shared<ContentIndex>();
        _contentIndex->setExtensions(exts);
        _contentIndex->setMaxFileSize(maxSize);
    }

    // Re-setup persistence with fresh index
    [self setupContentPersistence];

    // Re-index all files
    [self startContentIndexing];
}

// C-5: Accept engine parameter to avoid re-reading potentially stale _engine
- (void)updateContentIndexForPath:(const std::string&)fullPath
                          removed:(BOOL)removed
                           engine:(std::shared_ptr<SearchEngine>)engine {
    if (!engine || !_contentIndex) return;

    if (removed) {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            _contentIndex->removeFile(fileIndex);
            if (_contentPersistence) {
                _contentPersistence->walAppendRemove(fileIndex);
            }
        }
    } else {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            bool didIndex = _contentIndex->indexFile(fileIndex, fullPath);
            if (didIndex && _contentPersistence) {
                ContentFileInfo info;
                if (_contentIndex->getFileInfo(fileIndex, info)) {
                    _contentPersistence->walAppendAdd(fileIndex, info.contentHash, info.trigrams);
                }
            }
        }
    }
}

@end
