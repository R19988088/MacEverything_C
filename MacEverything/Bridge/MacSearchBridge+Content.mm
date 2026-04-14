#import "MacSearchBridge_Internal.h"
#include <dispatch/dispatch.h>
#include <unistd.h>

@implementation MacSearchBridge (Content)

// C-3: Thread-safe accessor for _contentIndex (mirrors safeEngine pattern)
- (std::shared_ptr<ContentIndex>)safeContentIndex {
    std::shared_lock lock(_contentMutex);
    return _contentIndex;
}

- (void)startContentIndexing {
    auto engine = [self safeEngine]; // C-4
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!engine || !contentIndex) return;
    _isContentIndexing.store(true, std::memory_order_relaxed);
    _cancelContentIndexing.store(false, std::memory_order_relaxed); // H-8: reset cancel flag

    auto contentPersistence = [self safeContentPersistence]; // C-3: thread-safe access
    auto* shuttingDown = &_shuttingDown;
    auto* cancelFlag = &_cancelContentIndexing; // H-8
    __weak MacSearchBridge *weakSelf = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        // Build a lightweight list of (index, fullPath) for regular files under one lock,
        // avoiding 4.5M getRecord() copies that each reconstruct path strings.
        struct FileEntry { uint32_t idx; std::string fullPath; };
        auto fileEntries = std::make_shared<std::vector<FileEntry>>();
        {
            uint32_t total = engine->recordCount();
            std::vector<uint32_t> allIndices;
            allIndices.reserve(total);
            for (uint32_t i = 0; i < total; i++) allIndices.push_back(i);

            fileEntries->reserve(total);
            engine->forEachRecordWithPath(allIndices, [&](uint32_t idx, const FileRecord& r, const std::string& path) {
                if (r.type != 1) return; // only regular files
                fileEntries->push_back({idx, SearchEngine::makeFullPath(path, r.name)});
            });
        }

        uint32_t total = static_cast<uint32_t>(fileEntries->size());
        auto indexed = std::make_shared<std::atomic<uint32_t>>(0);
        auto lastReported = std::make_shared<std::atomic<uint32_t>>(0);

        // H3 fix: Use dispatch_apply for parallel content indexing.
        // contentIndex->indexFile() is internally thread-safe (acquires its own lock).
        // contentPersistence->walAppendAdd() is also thread-safe (WAL has its own mutex).
        // This overlaps file I/O across threads for significant speedup.
        dispatch_queue_t concurrentQ = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        const auto& entries = *fileEntries;
        dispatch_apply(total, concurrentQ, ^(size_t i) {
            if (shuttingDown->load(std::memory_order_relaxed)) return;
            if (cancelFlag->load(std::memory_order_relaxed)) return;

            const auto& entry = entries[i];
            bool didIndex = contentIndex->indexFile(entry.idx, entry.fullPath);

            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(entry.idx, info)) {
                    contentPersistence->walAppendAdd(entry.idx, info.contentHash, info.trigrams);
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
                // P-5: Signal semaphore so rebuildContentIndex/prepareForTermination can wait
                dispatch_semaphore_signal(strongSelf->_contentIndexingSemaphore);
                if (strongSelf.onContentIndexComplete) {
                    strongSelf.onContentIndexComplete(totalIndexed);
                }
            }
        });
    });
}

- (void)setupContentPersistence {
    auto contentIndex = [self safeContentIndex]; // C-3: thread-safe access
    if (!contentIndex) return;

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

    auto newContentPersistence = std::make_shared<ContentIndexPersistence>(contentIndex, basePath, walPath);
    [self setContentPersistence:newContentPersistence]; // C-3: thread-safe assignment
    newContentPersistence->load();
    newContentPersistence->attachWAL();
    newContentPersistence->startAutoCompaction(300.0);
}

- (NSArray<MEContentResult *> *)queryContent:(NSString *)keyword maxResults:(uint32_t)maxResults {
    auto engine = [self safeEngine]; // C-4
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!engine || !contentIndex) return @[];

    std::string key([keyword UTF8String]);
    if (key.empty()) return @[];

    auto matches = contentIndex->query(key, maxResults);
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
        NSString *nsFileName = [NSString stringWithUTF8String:candidates[i].name.c_str()];
        NSString *nsFilePath = [NSString stringWithUTF8String:candidates[i].fullPath.c_str()];
        NSString *nsSnippet = [NSString stringWithUTF8String:snippetResults[i].snippet.c_str()];
        if (!nsFileName || !nsFilePath || !nsSnippet) continue; // H-1: skip non-UTF-8 file names/snippets
        MEContentResult *result = [[MEContentResult alloc]
            initWithFileName:nsFileName
                    filePath:nsFilePath
                     snippet:nsSnippet
                 matchOffset:snippetResults[i].offset
                    fileType:candidates[i].fileType];
        [results addObject:result];
    }

    return results;
}

- (void)setContentExtensions:(NSArray<NSString *> *)extensions {
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!contentIndex) return;
    std::vector<std::string> exts;
    exts.reserve(extensions.count);
    for (NSString *ext in extensions) {
        exts.push_back(std::string([ext UTF8String]));
    }
    contentIndex->setExtensions(exts);
}

- (void)setContentMaxFileSize:(uint64_t)bytes {
    auto contentIndex = [self safeContentIndex]; // C-3
    if (contentIndex) {
        contentIndex->setMaxFileSize(bytes);
    }
}

- (uint32_t)contentIndexedFileCount {
    auto contentIndex = [self safeContentIndex]; // C-3
    return contentIndex ? contentIndex->indexedFileCount() : 0;
}

- (NSArray<NSString *> *)contentGetExtensions {
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!contentIndex) return @[];
    auto exts = contentIndex->getExtensions();
    NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:exts.size()];
    for (const auto& ext : exts) {
        NSString *str = [NSString stringWithUTF8String:ext.c_str()];
        if (!str) continue;
        [result addObject:str];
    }
    return result;
}

- (uint64_t)contentGetMaxFileSize {
    auto contentIndex = [self safeContentIndex]; // C-3
    return contentIndex ? contentIndex->getMaxFileSize() : (1 * 1024 * 1024);
}

- (void)rebuildContentIndex {
    auto engine = [self safeEngine]; // C-4
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!engine || !contentIndex) return;

    // P-5: Cancel in-flight content indexing and wait via semaphore (not spin-wait)
    if (_isContentIndexing.load(std::memory_order_relaxed)) {
        _cancelContentIndexing.store(true, std::memory_order_relaxed);
        dispatch_semaphore_wait(_contentIndexingSemaphore,
                                dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        // C-2: Reset semaphore to drain accumulated signals from previous
        // startContentIndexing completions, preventing future waits from
        // passing through immediately.
        _contentIndexingSemaphore = dispatch_semaphore_create(0);
    }

    // H8 fix: Stop old persistence's auto-compaction timer before replacing,
    // to prevent timer firing into a dangling reference.
    // C-3: Thread-safe access via safeContentPersistence/setContentPersistence
    {
        auto oldContentPersistence = [self safeContentPersistence];
        if (oldContentPersistence) {
            oldContentPersistence->stopAutoCompactionAndWait();
        }
        [self setContentPersistence:nullptr];
    }

    // C-3: Replace _contentIndex under exclusive lock
    {
        auto exts = contentIndex->getExtensions();
        auto maxSize = contentIndex->getMaxFileSize();

        auto newIndex = std::make_shared<ContentIndex>();
        newIndex->setExtensions(exts);
        newIndex->setMaxFileSize(maxSize);

        std::unique_lock lock(_contentMutex);
        _contentIndex = newIndex;
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
    auto contentIndex = [self safeContentIndex]; // C-3
    if (!engine || !contentIndex) return;

    auto contentPersistence = [self safeContentPersistence]; // C-3: thread-safe access

    if (removed) {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            contentIndex->removeFile(fileIndex);
            if (contentPersistence) {
                contentPersistence->walAppendRemove(fileIndex);
            }
        }
    } else {
        uint32_t fileIndex = engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            bool didIndex = contentIndex->indexFile(fileIndex, fullPath);
            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(fileIndex, info)) {
                    contentPersistence->walAppendAdd(fileIndex, info.contentHash, info.trigrams);
                }
            }
        }
    }
}

@end
