#import "MacSearchBridge_Internal.h"
#include "DirectoryScanner.h"
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

@implementation MacSearchBridge

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
        _watcher = std::make_shared<FileSystemWatcher>();
        _contentIndex = std::make_shared<ContentIndex>();
        _isScanning.store(false, std::memory_order_relaxed);
        _isMonitoring.store(false, std::memory_order_relaxed);
        _isContentIndexing.store(false, std::memory_order_relaxed);
        _shuttingDown.store(false, std::memory_order_relaxed);
        _cancelContentIndexing.store(false, std::memory_order_relaxed);
        // H-7: Serial queue for mutations
        _mutationQueue = dispatch_queue_create("com.maceverything.mutation", DISPATCH_QUEUE_SERIAL);
        // P-5: Semaphore for waiting on content indexing completion
        _contentIndexingSemaphore = dispatch_semaphore_create(0);
    }
    return self;
}

// C-4: Thread-safe engine accessors
- (std::shared_ptr<SearchEngine>)safeEngine {
    std::shared_lock lock(_engineMutex);
    return _engine;
}

- (void)setEngine:(std::shared_ptr<SearchEngine>)engine {
    std::unique_lock lock(_engineMutex);
    _engine = engine;
}

// C-2: Thread-safe persistence accessors
- (std::shared_ptr<IndexPersistence>)safePersistence {
    std::shared_lock lock(_persistenceMutex);
    return _persistence;
}

- (void)setPersistence:(std::shared_ptr<IndexPersistence>)persistence {
    std::unique_lock lock(_persistenceMutex);
    _persistence = persistence;
}

// C-3: Thread-safe content persistence accessors
- (std::shared_ptr<ContentIndexPersistence>)safeContentPersistence {
    std::shared_lock lock(_contentPersistenceMutex);
    return _contentPersistence;
}

- (void)setContentPersistence:(std::shared_ptr<ContentIndexPersistence>)persistence {
    std::unique_lock lock(_contentPersistenceMutex);
    _contentPersistence = persistence;
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

        // Schedule scan timeout: cancel scanner after 45s
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC),
                       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            auto s = scannerWeak.lock();
            if (s && !s->isCancelled()) {
                NSLog(@"[MacSearchBridge] Scan timeout (45s) — cancelling scanner");
                s->cancel();
            }
        });

        scanner->scan(std::string([root UTF8String]));

        // Stop timer
        dispatch_source_cancel(timer);

        auto results = scanner->takeResults();

        auto engine = std::make_shared<SearchEngine>();
        engine->loadRecords(std::move(results));
        uint32_t count = engine->liveRecordCount();

        dispatch_async(dispatch_get_main_queue(), ^{
            [self setEngine:engine]; // C-4: thread-safe engine swap
            self->_isScanning.store(false, std::memory_order_relaxed);
            if (completion) {
                completion(count);
            }
            // Start file system monitoring after scan completes
            [self startMonitoringFrom:root];
            // Start content indexing in background (off main thread)
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                [self setupContentPersistence];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self startContentIndexing];
                });
            });
        });
    });
}

- (void)startIncrementalFrom:(NSString *)rootPath
                   cachePath:(NSString *)cachePath
                     walPath:(NSString *)walPath
                  completion:(void (^)(uint32_t totalRecords, BOOL didFullScan))completion {
    _isScanning.store(true, std::memory_order_relaxed);
    _startupCompleted.store(false, std::memory_order_relaxed);
    [self stopMonitoring];

    NSString *root = [rootPath copy];
    NSString *cache = [cachePath copy];
    NSString *wal = [walPath copy];

    // Global startup timeout: guarantee completion fires within 60s
    __weak MacSearchBridge *weakTimeoutSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        MacSearchBridge *strongSelf = weakTimeoutSelf;
        if (!strongSelf) return;
        bool expected = false;
        if (strongSelf->_startupCompleted.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            NSLog(@"[MacSearchBridge] Startup timeout (60s) — firing completion with 0 records");
            strongSelf->_isScanning.store(false, std::memory_order_relaxed);
            if (completion) completion(0, YES);
        }
    });

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
                    bool expected = false;
                    if (!self->_startupCompleted.compare_exchange_strong(expected, true,
                            std::memory_order_acq_rel)) {
                        return; // timeout already fired completion
                    }

                    [self setEngine:engine]; // C-4: thread-safe engine swap
                    [self setPersistence:sharedPersistence]; // C-2: thread-safe persistence swap
                    self->_isScanning.store(false, std::memory_order_relaxed);

                    sharedPersistence->attachWAL();
                    // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
                    sharedPersistence->setContentIndex([self safeContentIndex]);

                    uint32_t count = engine->liveRecordCount();
                    if (completion) completion(count, NO);

                    [self startMonitoringFrom:root];

                    sharedPersistence->startAutoCompaction(300.0, self->_watcher);

                    // Start content indexing in background
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                        [self setupContentPersistence];
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [self startContentIndexing];
                        });
                    });
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
                bool expected = false;
                if (!self->_startupCompleted.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel)) {
                    return; // timeout already fired completion
                }

                auto newPersistence = std::make_shared<IndexPersistence>(
                    [self safeEngine], cacheStr, walStr
                );
                [self setPersistence:newPersistence]; // C-2: thread-safe persistence swap
                newPersistence->attachWAL();
                // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
                newPersistence->setContentIndex([self safeContentIndex]);
                newPersistence->startAutoCompaction(300.0, self->_watcher);

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
                auto engine = [self safeEngine];
                if (engine) engine->saveToFile(cacheStr, meta);

                if (completion) completion(count, YES);
            }];
        });
    });
}

- (void)compactIndex {
    auto persistence = [self safePersistence]; // C-2: thread-safe access
    if (persistence && _watcher) {
        uint64_t eventId = _watcher->getLastEventId();
        // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
        persistence->setContentIndex([self safeContentIndex]);
        persistence->compact(eventId);
    }
}

- (void)prepareForTermination {
    // 1. Signal all background work to stop immediately
    _shuttingDown.store(true);
    _cancelContentIndexing.store(true, std::memory_order_relaxed); // C-4: cancel content indexing

    // 2. Capture last event ID before stopping the watcher
    uint64_t lastEventId = _watcher ? _watcher->getLastEventId() : 0;

    // 3. Stop FSEvents stream + auto-compaction timers and wait for in-flight handlers
    [self stopMonitoring];

    // C-4: Wait for content indexing dispatch_apply to finish before compacting.
    // _shuttingDown + _cancelContentIndexing are checked every iteration, so it will exit quickly.
    if (_isContentIndexing.load(std::memory_order_relaxed)) {
        dispatch_semaphore_wait(_contentIndexingSemaphore,
                                dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        // C-2: Reset semaphore to drain accumulated signals from previous
        // startContentIndexing completions.
        _contentIndexingSemaphore = dispatch_semaphore_create(0);
    }

    // 4. Now safe to compact on main thread — no competing lock holders
    auto persistence = [self safePersistence]; // C-2: thread-safe access
    if (persistence) {
        // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
        persistence->setContentIndex([self safeContentIndex]);
        persistence->compact(lastEventId);
    }
    auto contentPersistence = [self safeContentPersistence]; // C-3: thread-safe access
    if (contentPersistence) {
        contentPersistence->compact();
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
            // C-5: Pass engine to avoid re-reading stale _engine
            [self updateContentIndexForPath:path removed:YES engine:engine];
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

            // C-5: Pass engine to avoid re-reading stale _engine
            if (type == 1) {
                [self updateContentIndexForPath:path removed:NO engine:engine];
            }
        }
    }
}

- (void)startMonitoringFrom:(NSString *)rootPath {
    if (_isMonitoring.load(std::memory_order_relaxed)) return;

    std::string root([rootPath UTF8String]);

    // Capture engine as a shared_ptr for the callback
    __weak MacSearchBridge *weakSelf = self;

    // Exclude app's own cache directory from FSEvents to prevent self-triggering
    NSString *cacheDir = [NSString stringWithFormat:@"%@/Library/Caches/com.maceverything.app",
                          NSHomeDirectory()];
    _watcher->setExclusionPaths({std::string([cacheDir UTF8String])});

    _watcher->start(root, [weakSelf](std::vector<FileSystemWatcher::Event> events) {
        // This runs on the FSEvents serial queue (background)
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (strongSelf->_shuttingDown.load(std::memory_order_relaxed)) return;

        // C-4: Thread-safe engine access
        auto engine = [strongSelf safeEngine];
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
                // C-5: Pass engine to avoid re-reading stale _engine
                [strongSelf updateContentIndexForPath:path removed:YES engine:engine];
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

                // C-5: Pass engine to avoid re-reading stale _engine
                if (type == 1) { // regular file
                    [strongSelf updateContentIndexForPath:path removed:NO engine:engine];
                }
            }
        }

        // Trigger subtree rescans for MustScanSubDirs directories
        if (!rescanDirs.empty()) {
            for (const auto& dir : rescanDirs) {
                NSString *dirPath = [NSString stringWithUTF8String:dir.c_str()];
                if (!dirPath) continue;
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
    auto persistence = [self safePersistence]; // C-2: thread-safe access
    if (persistence) {
        persistence->stopAutoCompactionAndWait();
    }
    auto contentPersistence = [self safeContentPersistence]; // C-3: thread-safe access
    if (contentPersistence) {
        contentPersistence->stopAutoCompactionAndWait();
    }
    _isMonitoring.store(false, std::memory_order_relaxed);
}

- (NSArray<NSNumber *> *)queryIndices:(NSString *)keyword
                           maxResults:(uint32_t)maxResults {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = engine->query(key, maxResults);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

// P-3: Single engine lock for all indices instead of N+1 safeEngine calls
- (NSArray<MEFileResult *> *)recordsAtIndices:(NSArray<NSNumber *> *)indices {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return @[];

    // Convert NSArray to std::vector for forEachRecordWithPath
    std::vector<uint32_t> idxVec;
    idxVec.reserve(indices.count);
    for (NSNumber *num in indices) {
        idxVec.push_back([num unsignedIntValue]);
    }

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.count];
    engine->forEachRecordWithPath(idxVec, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return; // H-1: skip non-UTF-8 file names
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (NSArray<NSNumber *> *)recentIndices:(uint32_t)count {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return @[];

    auto indices = engine->recentIndices(count);

    NSMutableArray<NSNumber *> *result = [NSMutableArray arrayWithCapacity:indices.size()];
    for (uint32_t idx : indices) {
        [result addObject:@(idx)];
    }
    return result;
}

// P-4: Batch query+record lookup — eliminates NSNumber boxing and N+1 engine calls
- (NSArray<MEFileResult *> *)queryResults:(NSString *)keyword
                               maxResults:(uint32_t)maxResults {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return @[];

    std::string key([keyword UTF8String]);
    auto indices = engine->query(key, maxResults);
    if (indices.empty()) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.size()];
    engine->forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return; // H-1: skip non-UTF-8 file names
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

// P-4: Batch recent files — eliminates NSNumber boxing and N+1 engine calls
- (NSArray<MEFileResult *> *)recentResults:(uint32_t)count {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return @[];

    auto indices = engine->recentIndices(count);
    if (indices.empty()) return @[];

    NSMutableArray<MEFileResult *> *results = [NSMutableArray arrayWithCapacity:indices.size()];
    engine->forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
        NSString *nsName = [NSString stringWithUTF8String:r.name.c_str()];
        NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsName || !nsPath) return; // H-1: skip non-UTF-8 file names
        [results addObject:[[MEFileResult alloc] initWithName:nsName
                                                        path:nsPath
                                                        type:r.type
                                                        size:r.size
                                                     modTime:r.modTime]];
    });
    return results;
}

- (void)rescanSubtree:(NSString *)dirPath {
    // C-4: Thread-safe engine access
    auto engine = [self safeEngine];
    if (!engine) return;

    std::string dir([dirPath UTF8String]);
    auto* shuttingDown = &_shuttingDown;
    __weak MacSearchBridge *weakSelf = self;

    // H-7: Dispatch to serial mutation queue to prevent concurrent index mutations
    dispatch_async(_mutationQueue, ^{
        if (shuttingDown->load(std::memory_order_relaxed)) return;

        // Scan the subtree using DirectoryScanner
        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dir);
        auto freshRecords = scanner->takeResults();

        if (shuttingDown->load(std::memory_order_relaxed)) return;

        // Batch-replace: tombstone old prefix records + add fresh records + rebuild
        // trigram index once, instead of per-record remove+add (O(N²) → O(N))
        engine->batchRescanPrefix(dir, std::move(freshRecords));

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
    auto engine = [self safeEngine]; // C-4
    return engine ? engine->recordCount() : 0;
}

- (uint32_t)liveRecordCount {
    auto engine = [self safeEngine]; // C-4
    return engine ? engine->liveRecordCount() : 0;
}

- (BOOL)saveIndexToFile:(NSString *)path {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return NO;
    IndexMetadata meta;
    meta.lastEventId = _watcher ? _watcher->getLastEventId() : 0;
    meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
    NSOperatingSystemVersion osVer = [[NSProcessInfo processInfo] operatingSystemVersion];
    meta.extra[IndexMetadata::kOSVersion] = [[NSString stringWithFormat:@"%ld.%ld.%ld",
        (long)osVer.majorVersion, (long)osVer.minorVersion, (long)osVer.patchVersion] UTF8String];
    meta.extra[IndexMetadata::kRecordFormat] = "v3_inode";
    return engine->saveToFile(std::string([path UTF8String]), meta) ? YES : NO;
}

- (BOOL)loadIndexFromFile:(NSString *)path {
    auto engine = [self safeEngine]; // C-4
    if (!engine) return NO;
    return engine->loadFromFile(std::string([path UTF8String])) ? YES : NO;
}

@end
