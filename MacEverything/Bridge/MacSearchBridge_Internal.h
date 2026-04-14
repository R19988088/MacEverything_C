#pragma once
#import "MacSearchBridge.h"
#include "SearchEngine.h"
#include "ContentIndex.h"
#include "ContentIndexPersistence.h"
#include "IndexPersistence.h"
#include "FileSystemWatcher.h"
#include <memory>
#include <atomic>
#include <shared_mutex>

/// Class extension exposing ivars and internal methods
/// shared between MacSearchBridge.mm and MacSearchBridge+Content.mm.
@interface MacSearchBridge () {
@public
    std::shared_ptr<SearchEngine> _engine;
    std::unique_ptr<FileSystemWatcher> _watcher;
    std::shared_ptr<IndexPersistence> _persistence;
    std::shared_ptr<ContentIndex> _contentIndex;
    std::shared_ptr<ContentIndexPersistence> _contentPersistence;
    std::atomic<bool> _isScanning;
    std::atomic<bool> _isMonitoring;
    std::atomic<bool> _isContentIndexing;
    std::atomic<bool> _shuttingDown;
    std::atomic<bool> _startupCompleted;
    // C-4: Protects _engine shared_ptr from concurrent read/write
    std::shared_mutex _engineMutex;
    // H-7: Serial queue for index mutations (rescanSubtree, FSEvents)
    dispatch_queue_t _mutationQueue;
    // H-8: Cancellation flag for content indexing
    std::atomic<bool> _cancelContentIndexing;
}

/// Thread-safe engine accessor (C-4)
- (std::shared_ptr<SearchEngine>)safeEngine;

/// Content indexing lifecycle
- (void)startContentIndexing;
- (void)setupContentPersistence;

/// Update content index for a single path change (C-5: engine passed explicitly)
- (void)updateContentIndexForPath:(const std::string&)fullPath
                          removed:(BOOL)removed
                           engine:(std::shared_ptr<SearchEngine>)engine;

@end
