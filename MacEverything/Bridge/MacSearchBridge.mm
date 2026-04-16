#import "MacSearchBridge_Internal.h"
#import "MacSearchBridge+Content.h"
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

+ (void)initializeLogger {
    NSString *logDir = [NSString stringWithFormat:@"%@/Library/Logs/MacEverything",
                        NSHomeDirectory()];
#ifdef DEBUG
    me::Logger::instance().init(std::string([logDir UTF8String]), me::LogLevel::Debug);
#else
    me::Logger::instance().init(std::string([logDir UTF8String]), me::LogLevel::Info);
#endif
    LOG_INFO("App", "Logger initialized");
}

+ (void)logMessage:(NSString *)message level:(int)level module:(NSString *)module {
    me::LogLevel lvl;
    switch (level) {
        case 0:  lvl = me::LogLevel::Debug; break;
        case 1:  lvl = me::LogLevel::Info;  break;
        case 2:  lvl = me::LogLevel::Warn;  break;
        case 3:  lvl = me::LogLevel::Error; break;
        default: lvl = me::LogLevel::Info;  break;
    }
    me::Logger::instance().log(lvl, [module UTF8String], [message UTF8String]);
}

+ (NSString *)logFilePath {
    auto path = me::Logger::instance().getLogFilePath();
    return [NSString stringWithUTF8String:path.c_str()];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _engine = std::make_shared<SearchEngine>();
        _watcher = std::make_shared<FileSystemWatcher>("live");
        _contentIndex = std::make_shared<ContentIndex>();
        _isScanning.store(false, std::memory_order_relaxed);
        _isMonitoring.store(false, std::memory_order_relaxed);
        _isContentIndexing.store(false, std::memory_order_relaxed);
        _shuttingDown.store(false, std::memory_order_relaxed);
        _isSyncing.store(false, std::memory_order_relaxed);
        _cancelContentIndexing.store(false, std::memory_order_relaxed);
        _contentIndexGeneration.store(0, std::memory_order_relaxed);
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

- (BOOL)isSyncing {
    return _isSyncing.load(std::memory_order_relaxed);
}

- (void)startScanFrom:(NSString *)rootPath
           completion:(void (^)(uint32_t totalRecords))completion {
    _isScanning.store(true, std::memory_order_relaxed);

    // Stop existing monitoring during rescan
    [self stopMonitoring];

    NSString *root = [rootPath copy];

    LOG_INFO("Bridge", "startScanFrom: " << [root UTF8String]);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto scanStart = std::chrono::steady_clock::now();
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

        auto scanElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStart).count();
        LOG_INFO("Bridge", "Scan completed: " << count << " records in " << scanElapsed << "s");

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
    _isSyncing.store(false, std::memory_order_relaxed);
    [self stopMonitoring];

    NSString *root = [rootPath copy];
    NSString *cache = [cachePath copy];
    NSString *wal = [walPath copy];

    // R3-1: Acquire single-instance file lock to prevent WAL corruption
    //        from overlapping processes. Lock file lives next to the index files.
    {
        NSString *cacheDir = [NSString stringWithFormat:@"%@/Library/Caches/com.maceverything.app",
                              NSHomeDirectory()];
        // Ensure cache directory exists (lock file creation needs it)
        [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                                  withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:nil];
        std::string lockPath = std::string([cacheDir UTF8String]) + "/.instance.lock";
        if (!_instanceLock.tryLock(lockPath)) {
            LOG_WARN("Bridge", "Another instance may be running — proceeding with caution");
        }
    }

    LOG_INFO("Bridge", "startIncrementalFrom: " << [root UTF8String]);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto incrementalStart = std::chrono::steady_clock::now();
        auto engine = std::make_shared<SearchEngine>();
        std::string cacheStr([cache UTF8String]);
        std::string walStr([wal UTF8String]);
        // Derive paged persistence paths from cache directory
        std::string cacheDir = cacheStr.substr(0, cacheStr.rfind('/'));
        std::string pagesStr = cacheDir + "/index.pages";
        std::string ptableStr = cacheDir + "/index.ptable";
        auto persistence = std::make_unique<IndexPersistence>(
            engine, cacheStr, walStr, pagesStr, ptableStr
        );

        uint64_t lastEventId = persistence->load();
        uint32_t loadedCount = engine->liveRecordCount();

        if (lastEventId > 0 && loadedCount > 0) {
            // === Have cached index: deliver immediately, then sync in background ===
            auto sharedPersistence = std::shared_ptr<IndexPersistence>(std::move(persistence));

            dispatch_async(dispatch_get_main_queue(), ^{
                bool expected = false;
                if (!self->_startupCompleted.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel)) {
                    return; // should not happen here but guard
                }

                [self setEngine:engine]; // C-4: thread-safe engine swap
                [self setPersistence:sharedPersistence]; // C-2: thread-safe persistence swap
                self->_isScanning.store(false, std::memory_order_relaxed);
                self->_isSyncing.store(true, std::memory_order_relaxed);

                sharedPersistence->attachWAL();
                sharedPersistence->setContentIndex([self safeContentIndex]);

                uint32_t count = engine->liveRecordCount();
                if (completion) completion(count, NO);

                // === Background sync: FSEvents replay or full scan ===
                [self backgroundSyncEngine:engine
                               persistence:sharedPersistence
                               lastEventId:lastEventId
                                  cacheStr:cacheStr
                                    walStr:walStr
                                      root:root
                          incrementalStart:incrementalStart];
            });
            return;
        }

        // === No cache: full scan (no timeout — user tolerates first startup) ===
        dispatch_async(dispatch_get_main_queue(), ^{
            [self startScanFrom:root completion:^(uint32_t count) {
                bool expected = false;
                if (!self->_startupCompleted.compare_exchange_strong(expected, true,
                        std::memory_order_acq_rel)) {
                    return;
                }

                std::string freshCacheDir = cacheStr.substr(0, cacheStr.rfind('/'));
                std::string freshPagesStr = freshCacheDir + "/index.pages";
                std::string freshPtableStr = freshCacheDir + "/index.ptable";
                auto newPersistence = std::make_shared<IndexPersistence>(
                    [self safeEngine], cacheStr, walStr, freshPagesStr, freshPtableStr
                );
                [self setPersistence:newPersistence];
                newPersistence->attachWAL();
                newPersistence->setContentIndex([self safeContentIndex]);
                newPersistence->startAutoCompaction(300.0, self->_watcher);

                uint64_t eventId = self->_watcher ? self->_watcher->getLastEventId() : 0;
                IndexMetadata meta;
                meta.lastEventId = eventId;
                meta.extra[IndexMetadata::kScanRoot] = std::string([root UTF8String]);
                meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
                meta.extra[IndexMetadata::kRecordFormat] = "v4_paged";
                NSOperatingSystemVersion osVer = [[NSProcessInfo processInfo] operatingSystemVersion];
                meta.extra[IndexMetadata::kOSVersion] = [[NSString stringWithFormat:@"%ld.%ld.%ld",
                    (long)osVer.majorVersion, (long)osVer.minorVersion, (long)osVer.patchVersion] UTF8String];
                newPersistence->flush(meta, /*force=*/true);

                if (completion) completion(count, YES);
            }];
        });
    });
}

/// Background sync after delivering cached index. Tries FSEvents replay first;
/// falls back to full scan into the same engine via loadRecords().
- (void)backgroundSyncEngine:(std::shared_ptr<SearchEngine>)engine
                  persistence:(std::shared_ptr<IndexPersistence>)sharedPersistence
                  lastEventId:(uint64_t)lastEventId
                     cacheStr:(std::string)cacheStr
                       walStr:(std::string)walStr
                         root:(NSString *)root
             incrementalStart:(std::chrono::steady_clock::time_point)incrementalStart {

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // --- Try FSEvents replay ---
        auto replayDone = std::make_shared<std::atomic<bool>>(false);
        auto journalTruncated = std::make_shared<std::atomic<bool>>(false);

        __weak MacSearchBridge *weakSelf = self;
        auto watcherForReplay = std::make_unique<FileSystemWatcher>("replay");
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

        long result = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        watcherForReplay->stop();

        if (result == 0 && replayDone->load() && !journalTruncated->load()) {
            // --- Replay succeeded ---
            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - incrementalStart).count();
            LOG_INFO("Bridge", "Background replay succeeded: " << engine->liveRecordCount() << " records in " << elapsed << "s");

            dispatch_async(dispatch_get_main_queue(), ^{
                self->_isSyncing.store(false, std::memory_order_relaxed);
                [self startMonitoringFrom:root];
                sharedPersistence->startAutoCompaction(300.0, self->_watcher);

                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    [self setupContentPersistence];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [self startContentIndexing];
                    });
                });
                if (self.onIndexChanged) self.onIndexChanged();
            });
            return;
        }

        // --- Replay failed: background full scan into same engine ---
        LOG_WARN("Bridge", "FSEvents replay failed — background full scan into existing engine");

        auto scanner = std::make_shared<DirectoryScanner>();

        // Progress reporting
        std::weak_ptr<DirectoryScanner> scannerWeak = scanner;
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        __weak MacSearchBridge *weakTimerSelf = self;
        dispatch_source_set_event_handler(timer, ^{
            MacSearchBridge *strongSelf = weakTimerSelf;
            if (!strongSelf || !strongSelf.onScanProgress) return;
            auto s = scannerWeak.lock();
            if (!s) return;
            const auto& stats = s->getStats();
            strongSelf.onScanProgress(
                stats.fileCount.load(std::memory_order_relaxed),
                stats.dirCount.load(std::memory_order_relaxed)
            );
        });
        dispatch_resume(timer);

        scanner->scan(std::string([root UTF8String]));
        dispatch_source_cancel(timer);

        auto freshRecords = scanner->takeResults();
        // loadRecords uses unique_lock — blocks queries briefly but thread-safe
        engine->loadRecords(std::move(freshRecords));
        uint32_t finalCount = engine->liveRecordCount();

        auto scanElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - incrementalStart).count();
        LOG_INFO("Bridge", "Background scan completed: " << finalCount << " records in " << scanElapsed << "s");

        dispatch_async(dispatch_get_main_queue(), ^{
            self->_isSyncing.store(false, std::memory_order_relaxed);

            // Stop old persistence's auto-compaction before replacing
            sharedPersistence->stopAutoCompactionAndWait();

            // Replace persistence (old one destructs: detaches WAL, closes file)
            std::string bgCacheDir = cacheStr.substr(0, cacheStr.rfind('/'));
            std::string bgPagesStr = bgCacheDir + "/index.pages";
            std::string bgPtableStr = bgCacheDir + "/index.ptable";
            auto newPersistence = std::make_shared<IndexPersistence>(
                engine, cacheStr, walStr, bgPagesStr, bgPtableStr
            );
            [self setPersistence:newPersistence];
            newPersistence->attachWAL();
            newPersistence->setContentIndex([self safeContentIndex]);

            [self startMonitoringFrom:root];
            newPersistence->startAutoCompaction(300.0, self->_watcher);

            // Save snapshot via paged persistence
            uint64_t eventId = self->_watcher ? self->_watcher->getLastEventId() : 0;
            IndexMetadata meta;
            meta.lastEventId = eventId;
            meta.extra[IndexMetadata::kScanRoot] = std::string([root UTF8String]);
            meta.extra[IndexMetadata::kAppVersion] = "1.1.0";
            meta.extra[IndexMetadata::kRecordFormat] = "v4_paged";
            NSOperatingSystemVersion osVer = [[NSProcessInfo processInfo] operatingSystemVersion];
            meta.extra[IndexMetadata::kOSVersion] = [[NSString stringWithFormat:@"%ld.%ld.%ld",
                (long)osVer.majorVersion, (long)osVer.minorVersion, (long)osVer.patchVersion] UTF8String];
            newPersistence->flush(meta, /*force=*/true);

            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                [self setupContentPersistence];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self startContentIndexing];
                });
            });
            if (self.onIndexChanged) self.onIndexChanged();
        });
    });
}

- (void)compactIndex {
    LOG_INFO("Bridge", "compactIndex started");
    auto persistence = [self safePersistence]; // C-2: thread-safe access
    if (persistence && _watcher) {
        LOG_TIMER("Bridge", "compactIndex");
        uint64_t eventId = _watcher->getLastEventId();
        // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
        persistence->setContentIndex([self safeContentIndex]);
        persistence->compact(eventId);
    }
}

- (void)startHttpServer:(uint16_t)port {
    std::shared_lock lock(_engineMutex);
    if (!_httpServer) {
        _httpServer = std::make_shared<HttpServer>();
    }
    _httpServer->start(port, _engine, _contentIndex);

    // Inject admin callbacks so the HTTP API can trigger management operations
    __weak MacSearchBridge *weakSelf = self;
    HttpServer::AdminCallbacks callbacks;

    callbacks.onRebuildIndex = [weakSelf] {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"rebuildIndex" object:nil];
        });
    };

    callbacks.onRebuildContentIndex = [weakSelf] {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [strongSelf rebuildContentIndex];
        });
    };

    callbacks.onSetContentConfig = [weakSelf](const std::vector<std::string>& exts, uint64_t maxSize) {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        auto ci = [strongSelf safeContentIndex];
        if (!ci) return;
        ci->setExtensions(exts);
        ci->setMaxFileSize(maxSize);
    };

    callbacks.onGetContentExtensions = [weakSelf]() -> std::vector<std::string> {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return {};
        auto ci = [strongSelf safeContentIndex];
        if (!ci) return {};
        return ci->getExtensions();
    };

    callbacks.onGetContentMaxFileSize = [weakSelf]() -> uint64_t {
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return 0;
        auto ci = [strongSelf safeContentIndex];
        if (!ci) return 0;
        return ci->getMaxFileSize();
    };

    _httpServer->setAdminCallbacks(std::move(callbacks));
}

- (void)stopHttpServer {
    std::shared_lock lock(_engineMutex);
    if (_httpServer) {
        _httpServer->stop();
    }
}

- (void)prepareForTermination {
    [self stopHttpServer];
    LOG_INFO("Bridge", "prepareForTermination started");
    // 1. Signal all background work to stop immediately
    _shuttingDown.store(true);
    _cancelContentIndexing.store(true, std::memory_order_relaxed); // C-4: cancel content indexing

    // 2. Capture last event ID before stopping the watcher
    uint64_t lastEventId = _watcher ? _watcher->getLastEventId() : 0;

    // 3. Stop FSEvents stream + auto-compaction timers and wait for in-flight handlers
    [self stopMonitoring];

    // C-4: Wait for content indexing dispatch_apply to finish before compacting.
    // _shuttingDown + _cancelContentIndexing are checked every iteration, so it will exit quickly.
    // P0-1: Increment generation instead of replacing semaphore to avoid race
    _contentIndexGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (_isContentIndexing.load(std::memory_order_relaxed)) {
        dispatch_semaphore_wait(_contentIndexingSemaphore,
                                dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }

    // 4. Now safe to compact on main thread — no competing lock holders
    auto persistence = [self safePersistence]; // C-2: thread-safe access
    if (persistence) {
        // H-1: Set content index so compaction can propagate remap (C-3: safe accessor)
        persistence->setContentIndex([self safeContentIndex]);
        persistence->compact(lastEventId, /*force=*/true);
    }
    auto contentPersistence = [self safeContentPersistence]; // C-3: thread-safe access
    if (contentPersistence) {
        contentPersistence->compact(/*force=*/true);
    }
    LOG_INFO("Bridge", "prepareForTermination completed");
    LOG_INFO("Logger", "=== Log session ended ===");
    me::Logger::instance().shutdown();
}

// Rescan debounce constants
static constexpr double kRescanDebounceDelaySec = 5.0;
static constexpr double kRescanThrottleIntervalSec = 300.0;

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

        // Schedule debounced rescan for MustScanSubDirs directories
        if (!rescanDirs.empty()) {
            [strongSelf scheduleRescanForPaths:rescanDirs];
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

    // Cancel pending rescan debounce timer and clear state
    {
        std::lock_guard<std::mutex> lock(_pendingRescanMutex);
        if (_rescanDebounceTimer) {
            dispatch_source_cancel(_rescanDebounceTimer);
            _rescanDebounceTimer = nil;
        }
        _pendingRescanPaths.clear();
        _lastRescanTime.clear();
    }

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

- (void)scheduleRescanForPaths:(const std::vector<std::string>&)paths {
    std::lock_guard<std::mutex> lock(_pendingRescanMutex);

    // Merge new paths with pending, applying path subsumption
    _pendingRescanPaths = mergeRescanPaths(_pendingRescanPaths, paths);

    LOG_INFO("FSWatcher", "Debounce: " << _pendingRescanPaths.size()
             << " pending rescan path(s), scheduling " << kRescanDebounceDelaySec << "s delay");

    // Cancel existing timer (reset debounce window)
    if (_rescanDebounceTimer) {
        dispatch_source_cancel(_rescanDebounceTimer);
        _rescanDebounceTimer = nil;
    }

    // Create a new one-shot timer on the mutation queue
    _rescanDebounceTimer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _mutationQueue);

    uint64_t delaySec = static_cast<uint64_t>(kRescanDebounceDelaySec * NSEC_PER_SEC);
    dispatch_source_set_timer(_rescanDebounceTimer, dispatch_time(DISPATCH_TIME_NOW, delaySec),
                              DISPATCH_TIME_FOREVER, NSEC_PER_SEC / 10);

    __weak MacSearchBridge *weakSelf = self;
    dispatch_source_set_event_handler(_rescanDebounceTimer, ^{
        MacSearchBridge *strongSelf = weakSelf;
        if (strongSelf) {
            [strongSelf flushPendingRescans];
        }
    });
    dispatch_resume(_rescanDebounceTimer);
}

- (void)flushPendingRescans {
    // Take pending paths under lock
    std::set<std::string> pathsToRescan;
    std::set<std::string> throttledPaths;
    {
        std::lock_guard<std::mutex> lock(_pendingRescanMutex);
        for (const auto& path : _pendingRescanPaths) {
            if (shouldThrottleRescan(path, _lastRescanTime, kRescanThrottleIntervalSec)) {
                throttledPaths.insert(path);
                LOG_INFO("FSWatcher", "Throttled rescan for: " << path
                         << " (within " << kRescanThrottleIntervalSec << "s window)");
            } else {
                pathsToRescan.insert(path);
            }
        }
        _pendingRescanPaths = throttledPaths;

        // Cancel the timer — it already fired
        if (_rescanDebounceTimer) {
            dispatch_source_cancel(_rescanDebounceTimer);
            _rescanDebounceTimer = nil;
        }
    }

    if (pathsToRescan.empty() && throttledPaths.empty()) return;

    LOG_INFO("FSWatcher", "Flushing debounced rescan: " << pathsToRescan.size()
             << " path(s) to rescan, " << throttledPaths.size() << " throttled");

    // Execute rescans
    for (const auto& path : pathsToRescan) {
        NSString *dirPath = [NSString stringWithUTF8String:path.c_str()];
        if (!dirPath) continue;
        [self rescanSubtree:dirPath];

        // Record rescan time for throttle
        std::lock_guard<std::mutex> lock(_pendingRescanMutex);
        _lastRescanTime[path] = std::chrono::steady_clock::now();
    }

    // Clean up old entries in _lastRescanTime (> 2x throttle interval)
    {
        std::lock_guard<std::mutex> lock(_pendingRescanMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = _lastRescanTime.begin(); it != _lastRescanTime.end(); ) {
            auto elapsed = std::chrono::duration<double>(now - it->second).count();
            if (elapsed > kRescanThrottleIntervalSec * 2.0) {
                it = _lastRescanTime.erase(it);
            } else {
                ++it;
            }
        }
    }

    // If there are throttled paths, schedule a retry when throttle expires
    if (!throttledPaths.empty()) {
        std::lock_guard<std::mutex> lock(_pendingRescanMutex);
        if (!_rescanDebounceTimer) {
            _rescanDebounceTimer = dispatch_source_create(
                DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _mutationQueue);
            uint64_t delay = static_cast<uint64_t>(kRescanThrottleIntervalSec * NSEC_PER_SEC);
            dispatch_source_set_timer(_rescanDebounceTimer,
                                      dispatch_time(DISPATCH_TIME_NOW, delay),
                                      DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
            __weak MacSearchBridge *weakSelf = self;
            dispatch_source_set_event_handler(_rescanDebounceTimer, ^{
                MacSearchBridge *strongSelf = weakSelf;
                if (strongSelf) {
                    [strongSelf flushPendingRescans];
                }
            });
            dispatch_resume(_rescanDebounceTimer);
        }
    }
}

- (void)rescanSubtree:(NSString *)dirPath {
    // C-4: Thread-safe engine access
    auto engine = [self safeEngine];
    if (!engine) return;

    std::string dir([dirPath UTF8String]);
    __weak MacSearchBridge *weakSelf = self;

    // H-7: Dispatch to serial mutation queue to prevent concurrent index mutations
    dispatch_async(_mutationQueue, ^{
        MacSearchBridge *strongSelf = weakSelf;
        if (!strongSelf) return;
        if (strongSelf->_shuttingDown.load(std::memory_order_relaxed)) return;

        // Scan the subtree using DirectoryScanner
        auto scanner = std::make_shared<DirectoryScanner>();
        scanner->scan(dir);
        auto freshRecords = scanner->takeResults();

        strongSelf = weakSelf;
        if (!strongSelf) return;
        if (strongSelf->_shuttingDown.load(std::memory_order_relaxed)) return;

        // Batch-replace: tombstone old prefix records + add fresh records + rebuild
        // trigram index once, instead of per-record remove+add (O(N²) → O(N))
        engine->batchRescanPrefix(dir, std::move(freshRecords));

        // P0: Prevent unbounded vector growth from tombstone accumulation.
        // If tombstones exceed 30% of total records, compact in-place.
        uint32_t total = engine->recordCount();
        uint32_t live  = engine->liveRecordCount();
        if (total > live && (total - live) > total * 3 / 10) {
            auto remap = engine->compactRecords();
            if (!remap.empty() && strongSelf->_contentIndex) {
                strongSelf->_contentIndex->remapFileIndices(remap);
            }
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
