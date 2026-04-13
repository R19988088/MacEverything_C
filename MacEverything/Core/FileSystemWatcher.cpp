#include "FileSystemWatcher.h"
#include <cstring>

FileSystemWatcher::~FileSystemWatcher() {
    stop();
}

void FileSystemWatcher::start(const std::string& rootPath, Callback callback) {
    if (stream_) return;
    callback_ = std::move(callback);
    onReplayDone_ = nullptr;
    startInternal(rootPath, kFSEventStreamEventIdSinceNow);
}

void FileSystemWatcher::start(const std::string& rootPath,
                               FSEventStreamEventId sinceEventId,
                               Callback callback,
                               ReplayDoneCallback onReplayDone) {
    if (stream_) return;
    callback_ = std::move(callback);
    onReplayDone_ = std::move(onReplayDone);
    startInternal(rootPath, sinceEventId);
}

void FileSystemWatcher::startInternal(const std::string& rootPath,
                                       FSEventStreamEventId sinceEventId) {
    journalTruncated_.store(false, std::memory_order_relaxed);

    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault,
                                                  rootPath.c_str(),
                                                  kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(kCFAllocatorDefault,
                                             (const void**)&path, 1, &kCFTypeArrayCallBacks);

    FSEventStreamContext context = {};
    context.info = this;

    stream_ = FSEventStreamCreate(
        kCFAllocatorDefault,
        &FileSystemWatcher::fseventsCallback,
        &context,
        pathsToWatch,
        sinceEventId,
        0.3, // 300ms coalesce latency
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagUseCFTypes
    );

    CFRelease(pathsToWatch);
    CFRelease(path);

    if (!stream_) return;

    queue_ = dispatch_queue_create("com.maceverything.fswatcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream_, queue_);
    FSEventStreamStart(stream_);
}

void FileSystemWatcher::stop() {
    if (stream_) {
        FSEventStreamStop(stream_);
        FSEventStreamInvalidate(stream_);
        FSEventStreamRelease(stream_);
        stream_ = nullptr;
    }
    if (queue_) {
        dispatch_release(queue_);
        queue_ = nullptr;
    }
    callback_ = nullptr;
    onReplayDone_ = nullptr;
}

void FileSystemWatcher::fseventsCallback(
    ConstFSEventStreamRef /*streamRef*/,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[])
{
    auto* watcher = static_cast<FileSystemWatcher*>(clientCallBackInfo);

    // Track the latest event ID for persistence
    if (numEvents > 0) {
        watcher->lastEventId_.store(eventIds[numEvents - 1], std::memory_order_relaxed);
    }

    CFArrayRef paths = static_cast<CFArrayRef>(eventPaths);
    std::vector<Event> events;
    events.reserve(numEvents);

    bool historyDone = false;

    for (size_t i = 0; i < numEvents; i++) {
        FSEventStreamEventFlags flags = eventFlags[i];

        // Detect journal truncation — need full rescan
        if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
            watcher->journalTruncated_.store(true, std::memory_order_relaxed);
        }

        // Skip root-changed meta events
        if (flags & kFSEventStreamEventFlagRootChanged) continue;

        // Detect history-done — replay is complete
        if (flags & kFSEventStreamEventFlagHistoryDone) {
            historyDone = true;
            continue;
        }

        // Skip system directories that generate noise
        CFStringRef cfPath = static_cast<CFStringRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
        char pathBuf[PATH_MAX];
        if (!CFStringGetCString(cfPath, pathBuf, sizeof(pathBuf), kCFStringEncodingUTF8)) continue;

        std::string pathStr(pathBuf);

        // Filter out noisy system paths
        if (pathStr.find("/.Spotlight-V100/") != std::string::npos) continue;
        if (pathStr.find("/.fseventsd/") != std::string::npos) continue;
        if (pathStr.find("/.Trashes/") != std::string::npos) continue;
        if (pathStr.find("/private/var/folders/") != std::string::npos &&
            pathStr.find("/com.apple.") != std::string::npos) continue;

        events.push_back({std::move(pathStr), flags});
    }

    if (!events.empty() && watcher->callback_) {
        watcher->callback_(std::move(events));
    }

    // Fire replay-done callback after delivering any final events
    if (historyDone && watcher->onReplayDone_) {
        auto cb = std::move(watcher->onReplayDone_);
        watcher->onReplayDone_ = nullptr;
        cb();
    }
}
