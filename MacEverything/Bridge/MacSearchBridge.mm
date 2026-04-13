#import "MacSearchBridge.h"
#include "DirectoryScanner.h"
#include "SearchEngine.h"
#include "FileSystemWatcher.h"
#include "IndexPersistence.h"
#include "ContentIndex.h"
#include "ContentIndexPersistence.h"
#include <memory>
#include <atomic>
#include <sys/stat.h>

@implementation MEFileResult

- (instancetype)initWithName:(NSString *)name
                        path:(NSString *)path
                        type:(uint8_t)type
                        size:(uint64_t)size
                     modTime:(time_t)modTime {
    self = [super init];
    if (self) {
        _name = [name copy];
        _path = [path copy];
        _type = type;
        _size = size;
        _modTime = modTime;
    }
    return self;
}

@end

@implementation MEContentResult

- (instancetype)initWithFileName:(NSString *)fileName
                        filePath:(NSString *)filePath
                         snippet:(NSString *)snippet
                     matchOffset:(uint32_t)matchOffset
                        fileType:(uint8_t)fileType {
    self = [super init];
    if (self) {
        _fileName = [fileName copy];
        _filePath = [filePath copy];
        _snippet = [snippet copy];
        _matchOffset = matchOffset;
        _fileType = fileType;
    }
    return self;
}

@end

@implementation MacSearchBridge {
    std::shared_ptr<SearchEngine> _engine;
    std::unique_ptr<FileSystemWatcher> _watcher;
    std::shared_ptr<IndexPersistence> _persistence;
    std::shared_ptr<ContentIndex> _contentIndex;
    std::shared_ptr<ContentIndexPersistence> _contentPersistence;
    std::atomic<bool> _isScanning;
    std::atomic<bool> _isMonitoring;
    std::atomic<bool> _isContentIndexing;
    std::atomic<bool> _shuttingDown;
}

+ (instancetype)shared {
    static MacSearchBridge *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[MacSearchBridge alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _engine = std::make_shared<SearchEngine>();
        _watcher = std::make_unique<FileSystemWatcher>();
        _contentIndex = std::make_shared<ContentIndex>();
        _isScanning.store(false, std::memory_order_relaxed);
        _isMonitoring.store(false, std::memory_order_relaxed);
        _isContentIndexing.store(false, std::memory_order_relaxed);
        _shuttingDown.store(false, std::memory_order_relaxed);
    }
    return self;
}

- (BOOL)isScanning {
    return _isScanning.load(std::memory_order_relaxed);
}

- (BOOL)isMonitoring {
    return _isMonitoring.load(std::memory_order_relaxed);
}

- (void)startScanFrom:(NSString *)rootPath
           completion:(void (^)(uint32_t totalRecords))completion {
    _isScanning.store(true, std::memory_order_relaxed);

    // Stop existing monitoring during rescan
    [self stopMonitoring];

    NSString *root = [rootPath copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto scanner = std::make_shared<DirectoryScanner>();

        // Start progress polling timer
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        __weak MacSearchBridge *weakSelf = self;
        // B1: Use weak_ptr so the timer block doesn't keep the scanner alive
        std::weak_ptr<DirectoryScanner> scannerWeak = scanner;
        dispatch_source_set_event_handler(timer, ^{
            MacSearchBridge *strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.onScanProgress) return;
            auto scannerStrong = scannerWeak.lock();
            if (!scannerStrong) return;
            const auto& stats = scannerStrong->getStats();
            strongSelf.onScanProgress(
                stats.fileCount.load(std::memory_order_relaxed),
                stats.dirCount.load(std::memory_order_relaxed)
            );
        });
        dispatch_resume(timer);

        scanner->scan(std::string([root UTF8String]));

        // Stop timer
        dispatch_source_cancel(timer);

        auto results = scanner->takeResults();

        auto engine = std::make_shared<SearchEngine>();
        engine->loadRecords(std::move(results));
        uint32_t count = engine->liveRecordCount();

        dispatch_async(dispatch_get_main_queue(), ^{
            self->_engine = engine;
            self->_isScanning.store(false, std::memory_order_relaxed);
            if (completion) {
                completion(count);
            }
            // Start file system monitoring after scan completes
            [self startMonitoringFrom:root];
            // Start content indexing in background
            [self setupContentPersistence];
            [self startContentIndexing];
        });
    });
}

- (void)startIncrementalFrom:(NSString *)rootPath
                   cachePath:(NSString *)cachePath
                     walPath:(NSString *)walPath
                  completion:(void (^)(uint32_t totalRecords, BOOL didFullScan))completion {
    _isScanning.store(true, std::memory_order_relaxed);
    [self stopMonitoring];

    NSString *root = [rootPath copy];
    NSString *cache = [cachePath copy];
    NSString *wal = [walPath copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto engine = std::make_shared<SearchEngine>();
        auto persistence = std::make_unique<IndexPersistence>(
            engine,
            std::string([cache UTF8String]),
            std::string([wal UTF8String])
        );

        uint64_t lastEventId = persistence->load();
        uint32_t loadedCount = engine->liveRecordCount();

        if (lastEventId > 0 && loadedCount > 0) {
            // Have a cached index with a valid event ID — try FSEvents replay
            auto replayDone = std::make_shared<std::atomic<bool>>(false);
            auto journalTruncated = std::make_shared<std::atomic<bool>>(false);

            __weak MacSearchBridge *weakSelf = self;
            auto watcherForReplay = std::make_unique<FileSystemWatcher>();
            auto* watcherPtr = watcherForReplay.get();

            dispatch_semaphore_t sem = dispatch_semaphore_create(0);

            watcherPtr->start(
                std::string([root UTF8String]),
                lastEventId,
                [weakSelf, engine](std::vector<FileSystemWatcher::Event> events) {
                    MacSearchBridge *strongSelf = weakSelf;
                    if (!strongSelf) return;
                    [strongSelf applyFSEvents:events toEngine:engine];
                },
                [replayDone, journalTruncated, watcherPtr, sem] {
                    replayDone->store(true);
                    journalTruncated->store(watcherPtr->isJournalTruncated());
                    dispatch_semaphore_signal(sem);
                }
            );

            // Wait up to 10 seconds for replay to finish
            long result = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
            watcherForReplay->stop();

            if (result == 0 && replayDone->load() && !journalTruncated->load()) {
                // Replay succeeded — use the incrementally-updated index
                auto sharedPersistence = std::shared_ptr<IndexPersistence>(std::move(persistence));
                dispatch_async(dispatch_get_main_queue(), ^{
                    self->_engine = engine;
                    self->_persistence = sharedPersistence;
                    self->_isScanning.store(false, std::memory_order_relaxed);

                    self->_persistence->attachWAL();

                    uint32_t count = engine->liveRecordCount();
                    if (completion) completion(count, NO);

                    [self startMonitoringFrom:root];

                    self->_persistence->startAutoCompaction(300.0, self->_watcher.get());

                    // Start content indexing
                    [self setupContentPersistence];
                    [self startContentIndexing];
                });
                return;
            }
            // Replay failed or journal truncated — fall through to full scan
        }

        // Full scan fallback
        std::string cacheStr([cache UTF8String]);
        std::string walStr([wal UTF8String]);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self startScanFrom:root completion:^(uint32_t count) {
                self->_persistence = std::make_shared<IndexPersistence>(
                    self->_engine, cacheStr, walStr
                );
                self->_persistence->attachWAL();
                self->_persistence->startAutoCompaction(300.0, self->_watcher.get());

                uint64_t eventId = self->_watcher ? self->_watcher->getLastEventId() : 0;
                IndexMetadata meta;
                meta.lastEventId = eventId;
                meta.extra[IndexMetadata::kScanRoot] = std::string([root UTF8String]);
                meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
                meta.extra[IndexMetadata::kRecordFormat] = "v3_inode";
                // Capture OS version
                NSOperatingSystemVersion osVer = [[NSProcessInfo processInfo] operatingSystemVersion];
                meta.extra[IndexMetadata::kOSVersion] = [[NSString stringWithFormat:@"%ld.%ld.%ld",
                    (long)osVer.majorVersion, (long)osVer.minorVersion, (long)osVer.patchVersion] UTF8String];
                self->_engine->saveToFile(cacheStr, meta);

                if (completion) completion(count, YES);
            }];
        });
    });
}

- (void)compactIndex {
    if (_persistence && _watcher) {
        uint64_t eventId = _watcher->getLastEventId();
        _persistence->compact(eventId);
    }
}

- (void)prepareForTermination {
    // 1. Signal all background work to stop immediately
    _shuttingDown.store(true);

    // 2. Capture last event ID before stopping the watcher
    uint64_t lastEventId = _watcher ? _watcher->getLastEventId() : 0;

    // 3. Stop FSEvents stream + auto-compaction timers and wait for in-flight handlers
    [self stopMonitoring];

    // 4. Now safe to compact on main thread — no competing lock holders
    if (_persistence) {
        _persistence->compact(lastEventId);
    }
    if (_contentPersistence) {
        _contentPersistence->compact();
    }
}

/// Check if a path is inside a .app bundle (contains ".app/" as a path component boundary).
static bool isInsideAppBundle(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find(".app/", pos)) != std::string::npos) {
        // Ensure ".app" is at the end of a path component (preceded by some char that isn't '/')
        if (pos >= 1 && path[pos - 1] != '/') {
            return true;
        }
        pos += 5;
    }
    return false;
}

/// Check if a path ends with ".app".
static bool pathEndsWithApp(const std::string& path) {
    return path.size() > 4 &&
           path[path.size()-4] == '.' && path[path.size()-3] == 'a' &&
           path[path.size()-2] == 'p' && path[path.size()-1] == 'p';
}

- (void)applyFSEvents:(const std::vector<FileSystemWatcher::Event>&)events
              toEngine:(std::shared_ptr<SearchEngine>)engine {
    for (const auto& event : events) {
        const std::string& path = event.path;
        FSEventStreamEventFlags flags = event.flags;

        // Skip events inside .app bundles
        if (isInsideAppBundle(path)) continue;

        bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
        bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

        struct stat st;
        bool exists = (lstat(path.c_str(), &st) == 0);

        if (itemRemoved || (itemRenamed && !exists)) {
            // B2: Update content index before removing from metadata index
            [self updateContentIndexForPath:path removed:YES];
            engine->removeByPath(path);
        } else if (exists) {
            std::string dirPath, fileName;
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                dirPath = path.substr(0, lastSlash);
                fileName = path.substr(lastSlash + 1);
            } else {
                dirPath = ".";
                fileName = path;
            }
            if (fileName.empty()) continue;

            uint8_t type = 4;
            if (S_ISREG(st.st_mode))       type = 1;
            else if (S_ISDIR(st.st_mode)) {
                type = pathEndsWithApp(path) ? 5 : 2;
            }
            else if (S_ISLNK(st.st_mode))   type = 3;

            FileRecord record;
            record.name = fileName;
            record.path = dirPath;
            record.type = type;
            record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
            record.modTime = st.st_mtime;
            record.inode = st.st_ino;
            record.devId = static_cast<int32_t>(st.st_dev);

            engine->updateByPath(path, std::move(record));

            // B2: Update content index for created/modified regular files
            if (type == 1) {
                [self updateContentIndexForPath:path removed:NO];
            }
        }
    }
}

- (void)startMonitoringFrom:(NSString *)rootPath {
    if (_isMonitoring.load(std::memory_order_relaxed)) return;

    std::string root([rootPath UTF8String]);

    // Capture engine as a shared_ptr for the callback
    __weak MacSearchBridge *weakSelf = self;

    auto* shuttingDownPtr = &_shuttingDown;
    _watcher->start(root, [weakSelf, shuttingDownPtr](std::vector<FileSystemWatcher::Event> events) {
        // This runs on the FSEvents serial queue (background)
        if (shuttingDownPtr->load(std::memory_order_relaxed)) return;

        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;

        auto engine = strongSelf->_engine;
        if (!engine) return;

        // Collect directories that need a full rescan (MustScanSubDirs)
        std::vector<std::string> rescanDirs;

        bool changed = false;

        for (const auto& event : events) {
            const std::string& path = event.path;
            FSEventStreamEventFlags flags = event.flags;

            // Skip events inside .app bundles
            if (isInsideAppBundle(path)) continue;

            // Handle MustScanSubDirs: FSEvents journal was truncated for this path,
            // individual file events may have been lost — rescan the entire subtree.
            if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
                rescanDirs.push_back(path);
                continue;
            }

            // Determine what happened
            bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
            bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

            // stat the path to determine current state
            struct stat st;
            bool exists = (lstat(path.c_str(), &st) == 0);

            if (itemRemoved || (itemRenamed && !exists)) {
                // File was removed or renamed away
                // Update content index before removing from metadata index
                [strongSelf updateContentIndexForPath:path removed:YES];
                if (engine->removeByPath(path)) {
                    changed = true;
                }
            } else if (exists) {
                // File was created, renamed in, or modified
                // Extract directory and filename components
                std::string dirPath, fileName;
                size_t lastSlash = path.rfind('/');
                if (lastSlash != std::string::npos) {
                    dirPath = path.substr(0, lastSlash);
                    fileName = path.substr(lastSlash + 1);
                } else {
                    dirPath = ".";
                    fileName = path;
                }

                if (fileName.empty()) continue;

                // Determine type
                uint8_t type = 4; // other
                if (S_ISREG(st.st_mode))       type = 1;
                else if (S_ISDIR(st.st_mode))   type = pathEndsWithApp(fileName) ? 5 : 2;
                else if (S_ISLNK(st.st_mode))   type = 3;

                FileRecord record;
                record.name = fileName;
                record.path = dirPath;
                record.type = type;
                record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
                record.modTime = st.st_mtime;
                record.inode = st.st_ino;
                record.devId = static_cast<int32_t>(st.st_dev);

                engine->updateByPath(path, std::move(record));
                changed = true;

                // Update content index for modified/created files
                if (type == 1) { // regular file
                    [strongSelf updateContentIndexForPath:path removed:NO];
                }
            }
        }

        // Trigger subtree rescans for MustScanSubDirs directories
        if (!rescanDirs.empty()) {
            for (const auto& dir : rescanDirs) {
                NSString *dirPath = [NSString stringWithUTF8String:dir.c_str()];
                [strongSelf rescanSubtree:dirPath];
            }
            // rescanSubtree will notify UI on completion, no need to set changed here
        }

        if (changed) {
            dispatch_async(dispatch_get_main_queue(), ^{
                MacSearchBridge *strongSelf2 = weakSelf;
                if (strongSelf2 && strongSelf2.onIndexChanged) {
                    strongSelf2.onIndexChanged();
                }
            });
        }
    });

    _isMonitoring.store(true, std::memory_order_relaxed);
}

- (void)stopMonitoring {
    // Stop FSEvents first — prevents new lock acquisitions from callbacks
    _watcher->stop();

    // Stop auto-compaction timers and wait for any in-flight handlers to finish.
    // This ensures no background thread holds the mutex when we return.
    if (_persistence) {
        _persistence->stopAutoCompactionAndWait();
    }
    if (_contentPersistence) {
        _contentPersistence->stopAutoCompactionAndWait();
    }
    _isMonitoring.store(false, std::memory_order_relaxed);
}

- (NSArray<NSNumber *> *)queryIndices:(NSString *)keyword
                           maxResults:(uint32_t)maxResults {
    if (!_engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = _engine->query(key, maxResults);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

- (MEFileResult *)recordAtIndex:(uint32_t)index {
    if (!_engine || index >= _engine->recordCount()) return nil;

    const auto& r = _engine->getRecord(index);
    // Skip tombstoned records
    if (r.type == 0) return nil;

    return [[MEFileResult alloc] initWithName:[NSString stringWithUTF8String:r.name.c_str()]
                                        path:[NSString stringWithUTF8String:r.path.c_str()]
                                        type:r.type
                                        size:r.size
                                     modTime:r.modTime];
}

- (NSArray<MEFileResult *> *)recordsAtIndices:(NSArray<NSNumber *> *)indices {
    if (!_engine) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.count];
    for (NSNumber *num in indices) {
        uint32_t idx = [num unsignedIntValue];
        MEFileResult *r = [self recordAtIndex:idx];
        if (r) [results addObject:r];
    }
    return results;
}

- (NSArray<NSNumber *> *)recentIndices:(uint32_t)count {
    if (!_engine) return @[];

    // B4: Use engine's batch method — single lock, no per-record overhead
    auto indices = _engine->recentIndices(count);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

- (void)rescanSubtree:(NSString *)dirPath {
    if (!_engine) return;

    std::string dir([dirPath UTF8String]);
    auto engine = _engine;
    auto* shuttingDown = &_shuttingDown;
    __weak MacSearchBridge *weakSelf = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (shuttingDown->load(std::memory_order_relaxed)) return;

        // Scan the subtree using DirectoryScanner
        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dir);
        auto freshRecords = scanner->takeResults();

        if (shuttingDown->load(std::memory_order_relaxed)) return;

        // Remove all old records under this directory prefix
        engine->removeByPathPrefix(dir);

        // Add fresh records
        for (auto& record : freshRecords) {
            if (shuttingDown->load(std::memory_order_relaxed)) return;
            std::string fullPath = record.path;
            if (!fullPath.empty() && fullPath.back() != '/') fullPath += "/";
            fullPath += record.name;
            engine->updateByPath(fullPath, std::move(record));
        }

        // Notify UI
        dispatch_async(dispatch_get_main_queue(), ^{
            MacSearchBridge *strongSelf = weakSelf;
            if (strongSelf && strongSelf.onIndexChanged) {
                strongSelf.onIndexChanged();
            }
        });
    });
}

- (uint32_t)recordCount {
    return _engine ? _engine->recordCount() : 0;
}

- (uint32_t)liveRecordCount {
    return _engine ? _engine->liveRecordCount() : 0;
}

- (BOOL)saveIndexToFile:(NSString *)path {
    if (!_engine) return NO;
    // B3: Preserve metadata (event ID, scan root, versions) when saving
    IndexMetadata meta;
    meta.lastEventId = _watcher ? _watcher->getLastEventId() : 0;
    meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
    NSOperatingSystemVersion osVer = [[NSProcessInfo processInfo] operatingSystemVersion];
    meta.extra[IndexMetadata::kOSVersion] = [[NSString stringWithFormat:@"%ld.%ld.%ld",
        (long)osVer.majorVersion, (long)osVer.minorVersion, (long)osVer.patchVersion] UTF8String];
    meta.extra[IndexMetadata::kRecordFormat] = "v3_inode";
    return _engine->saveToFile(std::string([path UTF8String]), meta) ? YES : NO;
}

- (BOOL)loadIndexFromFile:(NSString *)path {
    if (!_engine) return NO;
    return _engine->loadFromFile(std::string([path UTF8String])) ? YES : NO;
}

// --- Content search ---

- (void)startContentIndexing {
    if (!_engine || !_contentIndex) return;
    _isContentIndexing.store(true, std::memory_order_relaxed);

    auto engine = _engine;
    auto contentIndex = _contentIndex;
    auto contentPersistence = _contentPersistence;
    auto* shuttingDown = &_shuttingDown;
    __weak MacSearchBridge *weakSelf = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        uint32_t total = engine->recordCount();
        std::atomic<uint32_t> indexed{0};
        std::atomic<uint32_t> lastReported{0};

        for (uint32_t i = 0; i < total; i++) {
            // Bail out immediately if the app is shutting down
            if (shuttingDown->load(std::memory_order_relaxed)) return;

            auto record = engine->getRecord(i);
            if (record.type != 1) continue; // only regular files

            std::string fullPath = SearchEngine::fullPathForRecord(record.path, record.name);
            bool didIndex = contentIndex->indexFile(i, fullPath);

            if (didIndex && contentPersistence) {
                ContentFileInfo info;
                if (contentIndex->getFileInfo(i, info)) {
                    contentPersistence->walAppendAdd(i, info.contentHash, info.trigrams);
                }
            }

            uint32_t current = indexed.fetch_add(1, std::memory_order_relaxed) + 1;

            // Report progress every 500 files
            if (current - lastReported.load(std::memory_order_relaxed) >= 500) {
                lastReported.store(current, std::memory_order_relaxed);
                uint32_t c = current;
                dispatch_async(dispatch_get_main_queue(), ^{
                    MacSearchBridge *strongSelf = weakSelf;
                    if (strongSelf && strongSelf.onContentIndexProgress) {
                        strongSelf.onContentIndexProgress(c, total);
                    }
                });
            }
        }

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
    if (!_engine || !_contentIndex) return @[];

    std::string key([keyword UTF8String]);
    if (key.empty()) return @[];

    auto matches = _contentIndex->query(key, maxResults);

    NSMutableArray<MEContentResult *> *results = [NSMutableArray arrayWithCapacity:matches.size()];

    for (const auto& match : matches) {
        auto record = _engine->getRecord(match.fileIndex);
        if (record.type == 0) continue; // skip tombstones

        std::string fullPath = SearchEngine::fullPathForRecord(record.path, record.name);

        // Generate snippet on the fly
        uint32_t offset = 0;
        std::string snippet = ContentIndex::generateSnippet(fullPath, key, offset);

        if (snippet.empty()) continue; // false positive from trigram index

        MEContentResult *result = [[MEContentResult alloc]
            initWithFileName:[NSString stringWithUTF8String:record.name.c_str()]
                    filePath:[NSString stringWithUTF8String:fullPath.c_str()]
                     snippet:[NSString stringWithUTF8String:snippet.c_str()]
                 matchOffset:offset
                    fileType:record.type];
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
    if (!_engine || !_contentIndex) return;
    if (_isContentIndexing.load(std::memory_order_relaxed)) return; // already running

    // Clear old content index data
    {
        // Remove all indexed files from ContentIndex
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

- (void)updateContentIndexForPath:(const std::string&)fullPath removed:(BOOL)removed {
    if (!_engine || !_contentIndex) return;

    if (removed) {
        uint32_t fileIndex = _engine->indexForPath(fullPath);
        if (fileIndex != UINT32_MAX) {
            _contentIndex->removeFile(fileIndex);
            if (_contentPersistence) {
                _contentPersistence->walAppendRemove(fileIndex);
            }
        }
    } else {
        uint32_t fileIndex = _engine->indexForPath(fullPath);
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
