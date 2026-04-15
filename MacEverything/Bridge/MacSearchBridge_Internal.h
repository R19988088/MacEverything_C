#pragma once
#import "MacSearchBridge.h"
#include "Logger.h"
#include "SearchEngine.h"
#include "ContentIndex.h"
#include "ContentIndexPersistence.h"
#include "IndexPersistence.h"
#include "FileSystemWatcher.h"
#include "InstanceLock.h"
#include "RescanDebounce.h"
#include <memory>
#include <atomic>
#include <shared_mutex>
#include <set>
#include <chrono>
#include <dispatch/dispatch.h>

/// Class extension exposing ivars and internal methods
/// shared between MacSearchBridge.mm and MacSearchBridge+Content.mm.
@interface MacSearchBridge () {
@public
    std::shared_ptr<SearchEngine> _engine;
    std::shared_ptr<FileSystemWatcher> _watcher; // H9: shared_ptr so compaction timer can safely reference
    std::shared_ptr<IndexPersistence> _persistence;
    std::shared_ptr<ContentIndex> _contentIndex;
    std::shared_ptr<ContentIndexPersistence> _contentPersistence;
    std::atomic<bool> _isScanning;
    std::atomic<bool> _isMonitoring;
    std::atomic<bool> _isContentIndexing;
    std::atomic<bool> _shuttingDown;
    std::atomic<bool> _startupCompleted;
    std::atomic<bool> _isSyncing;      // index loaded & searchable, background sync in progress
    // C-4: Protects _engine shared_ptr from concurrent read/write
    std::shared_mutex _engineMutex;
    // H-7: Serial queue for index mutations (rescanSubtree, FSEvents)
    dispatch_queue_t _mutationQueue;
    // H-8: Cancellation flag for content indexing
    std::atomic<bool> _cancelContentIndexing;
    // C-3: Protects _contentIndex shared_ptr from concurrent read/write
    std::shared_mutex _contentMutex;
    // C-2: Protects _persistence shared_ptr from concurrent read/write
    std::shared_mutex _persistenceMutex;
    // C-3: Protects _contentPersistence shared_ptr from concurrent read/write
    std::shared_mutex _contentPersistenceMutex;
    // P-5: Semaphore signaled when content indexing completes
    dispatch_semaphore_t _contentIndexingSemaphore;
    // P0-1: Generation counter to detect stale content indexing completions
    std::atomic<uint64_t> _contentIndexGeneration;
    // R3-1: Single-instance file lock to prevent WAL corruption from overlapping processes
    InstanceLock _instanceLock;
    // Rescan debounce state
    std::mutex _pendingRescanMutex;
    std::set<std::string> _pendingRescanPaths;
    dispatch_source_t _rescanDebounceTimer;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> _lastRescanTime;
}

/// Thread-safe engine accessor (C-4)
- (std::shared_ptr<SearchEngine>)safeEngine;

/// Thread-safe content index accessor (C-3)
- (std::shared_ptr<ContentIndex>)safeContentIndex;

/// Thread-safe persistence accessors (C-2)
- (std::shared_ptr<IndexPersistence>)safePersistence;
- (void)setPersistence:(std::shared_ptr<IndexPersistence>)persistence;

/// Thread-safe content persistence accessors (C-3)
- (std::shared_ptr<ContentIndexPersistence>)safeContentPersistence;
- (void)setContentPersistence:(std::shared_ptr<ContentIndexPersistence>)persistence;

/// Content indexing lifecycle
- (void)startContentIndexing;
- (void)setupContentPersistence;

/// Update content index for a single path change (C-5: engine passed explicitly)
- (void)updateContentIndexForPath:(const std::string&)fullPath
                          removed:(BOOL)removed
                           engine:(std::shared_ptr<SearchEngine>)engine;

@end
