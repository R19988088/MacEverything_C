#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Lightweight wrapper exposing a single file/directory result to Swift.
@interface MEFileResult : NSObject
@property (nonatomic, readonly) NSString *name;
@property (nonatomic, readonly) NSString *path;
@property (nonatomic, readonly) uint8_t type;      // 1=file, 2=dir, 3=symlink, 4=other
@property (nonatomic, readonly) uint64_t size;
@property (nonatomic, readonly) time_t modTime;
- (instancetype)initWithName:(NSString *)name
                        path:(NSString *)path
                        type:(uint8_t)type
                        size:(uint64_t)size
                     modTime:(time_t)modTime;
@end

/// Lightweight wrapper exposing a content search result to Swift.
@interface MEContentResult : NSObject
@property (nonatomic, readonly) NSString *fileName;
@property (nonatomic, readonly) NSString *filePath;
@property (nonatomic, readonly) NSString *snippet;      // matched context with ellipsis
@property (nonatomic, readonly) uint32_t matchOffset;    // byte offset of match in file
@property (nonatomic, readonly) uint8_t fileType;        // 1=file, 2=dir, 3=symlink, 4=other
- (instancetype)initWithFileName:(NSString *)fileName
                        filePath:(NSString *)filePath
                         snippet:(NSString *)snippet
                     matchOffset:(uint32_t)matchOffset
                        fileType:(uint8_t)fileType;
@end

/// Bridge between the C++ search engine and Swift/SwiftUI.
@interface MacSearchBridge : NSObject

+ (instancetype)shared;

/// Start a full-disk scan on a background queue.
/// The completion block is called on the main queue when done.
/// File system monitoring starts automatically after scan completes.
- (void)startScanFrom:(NSString *)rootPath
           completion:(void (^)(uint32_t totalRecords))completion;

/// Start with incremental loading: load cached index + WAL, replay FSEvents since last save.
/// Falls back to full scan if FSEvents journal is unavailable.
/// Completion reports total records and whether a full scan was needed.
- (void)startIncrementalFrom:(NSString *)rootPath
                   cachePath:(NSString *)cachePath
                     walPath:(NSString *)walPath
                  completion:(void (^)(uint32_t totalRecords, BOOL didFullScan))completion;

/// Compact the index: write new base snapshot, clear WAL.
- (void)compactIndex;

/// Perform a case-insensitive substring search. Returns matching record indices.
- (NSArray<NSNumber *> *)queryIndices:(NSString *)keyword
                           maxResults:(uint32_t)maxResults;

/// Fetch a single record by index.
- (nullable MEFileResult *)recordAtIndex:(uint32_t)index;

/// Fetch multiple records by indices (batch).
- (NSArray<MEFileResult *> *)recordsAtIndices:(NSArray<NSNumber *> *)indices;

/// Return indices of the most recently modified files, sorted by modTime descending.
- (NSArray<NSNumber *> *)recentIndices:(uint32_t)count;

/// P-4: Perform query and return results directly, eliminating NSNumber boxing overhead.
- (NSArray<MEFileResult *> *)queryResults:(NSString *)keyword maxResults:(uint32_t)maxResults;

/// P-4: Return most recently modified files as results directly.
- (NSArray<MEFileResult *> *)recentResults:(uint32_t)count;

/// Total number of indexed records (including tombstones).
- (uint32_t)recordCount;

/// Number of live (non-tombstoned) records.
- (uint32_t)liveRecordCount;

/// Stop file system monitoring.
- (void)stopMonitoring;

/// Gracefully shut down: stop monitoring, then compact index.
/// Call this from applicationWillTerminate instead of compactIndex directly.
- (void)prepareForTermination;

/// Save the current index to a binary file.
- (BOOL)saveIndexToFile:(NSString *)path;

/// Load index from a binary file. Returns YES if successful.
- (BOOL)loadIndexFromFile:(NSString *)path;

/// Rescan a directory subtree and update the index incrementally.
- (void)rescanSubtree:(NSString *)dirPath;

// --- Content search ---

/// Search file contents for the given keyword. Returns content results with snippets.
- (NSArray<MEContentResult *> *)queryContent:(NSString *)keyword maxResults:(uint32_t)maxResults;

/// Set allowed file extensions for content indexing (lowercase, without dot).
- (void)setContentExtensions:(NSArray<NSString *> *)extensions;

/// Set maximum file size for content indexing (bytes).
- (void)setContentMaxFileSize:(uint64_t)bytes;

/// Number of content-indexed files.
- (uint32_t)contentIndexedFileCount;

/// Get current content indexing extensions.
- (NSArray<NSString *> *)contentGetExtensions;

/// Get current max file size for content indexing (bytes).
- (uint64_t)contentGetMaxFileSize;

/// Rebuild content index with current settings (clears old index, re-indexes all files).
- (void)rebuildContentIndex;

/// Whether a scan is currently in progress.
@property (nonatomic, readonly) BOOL isScanning;

/// Whether file system monitoring is active.
@property (nonatomic, readonly) BOOL isMonitoring;

/// Called on the main queue when file system changes are applied to the index.
@property (nonatomic, copy, nullable) void (^onIndexChanged)(void);

/// Called on the main queue periodically during scanning with progress counts.
@property (nonatomic, copy, nullable) void (^onScanProgress)(uint64_t fileCount, uint64_t dirCount);

/// Called on the main queue periodically during content indexing with progress.
@property (nonatomic, copy, nullable) void (^onContentIndexProgress)(uint32_t indexed, uint32_t total);

/// Called on the main queue when content indexing completes.
@property (nonatomic, copy, nullable) void (^onContentIndexComplete)(uint32_t totalIndexed);

@end

NS_ASSUME_NONNULL_END
