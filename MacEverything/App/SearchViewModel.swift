import Foundation
import Combine

struct FileItem: Identifiable {
    let id: UInt32      // index in the engine
    let name: String
    let path: String
    let type: UInt8
    let size: UInt64
    let modTime: time_t
}

struct ContentFileItem: Identifiable {
    let id = UUID()
    let fileName: String
    let filePath: String
    let snippet: String
    let matchOffset: UInt32
    let fileType: UInt8
}

@MainActor
class SearchViewModel: ObservableObject {
    @Published var searchText: String = ""
    @Published var displayItems: [FileItem] = []
    @Published var totalMatches: Int = 0
    @Published var isScanning: Bool = false
    @Published var scanComplete: Bool = false
    @Published var totalRecords: UInt32 = 0
    @Published var queryTimeMs: Double = 0
    @Published var isMonitoring: Bool = false
    @Published var scannedCount: UInt64 = 0
    @Published var isLoadingMore: Bool = false
    @Published var showingRecent: Bool = false
    @Published var isContentSearch: Bool = false
    @Published var contentResults: [ContentFileItem] = []
    @Published var isContentIndexing: Bool = false
    @Published var contentIndexProgress: (indexed: UInt32, total: UInt32)?

    private let bridge = MacSearchBridge.shared()
    private var searchTask: Task<Void, Never>?
    private var recentTask: Task<Void, Never>?
    private var cachedIndices: [NSNumber] = []
    private var loadedCount: Int = 0
    private var searchGeneration: UInt64 = 0

    private static let pageSize: Int = 100
    private static let maxResults: UInt32 = 10000
    private static let indexChangeThrottleNs: UInt64 = 2_000_000_000 // 2 seconds

    private var indexChangeTask: Task<Void, Never>?
    private var indexChangePending = false

    static var cacheDir: String {
        let base = NSSearchPathForDirectoriesInDomains(
            .cachesDirectory, .userDomainMask, true
        ).first ?? NSTemporaryDirectory()
        let appCache = (base as NSString).appendingPathComponent("com.maceverything.app")
        try? FileManager.default.createDirectory(
            atPath: appCache, withIntermediateDirectories: true
        )
        return appCache
    }

    static var cachePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.bin")
    }

    static var walPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.wal")
    }

    init() {
        startIncremental()
    }

    func startIncremental() {
        isScanning = true
        scannedCount = 0

        bridge.onScanProgress = { [weak self] fileCount, dirCount in
            Task { @MainActor in
                self?.scannedCount = fileCount + dirCount
            }
        }

        bridge.onContentIndexProgress = { [weak self] indexed, total in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = true
                self.contentIndexProgress = (indexed, total)
            }
        }

        bridge.onContentIndexComplete = { [weak self] totalIndexed in
            Task { @MainActor in
                guard let self = self else { return }
                self.isContentIndexing = false
                self.contentIndexProgress = nil
                // Auto-refresh content search results after indexing completes
                if self.isContentSearch && !self.contentKeyword.isEmpty {
                    self.performContentSearch(self.contentKeyword)
                }
            }
        }

        // Set up FSEvents change callback before starting
        bridge.onIndexChanged = { [weak self] in
            Task { @MainActor in
                self?.onIndexChanged()
            }
        }

        bridge.startIncremental(from: "/",
                                cachePath: Self.cachePath,
                                walPath: Self.walPath) { [weak self] count, didFullScan in
            Task { @MainActor in
                guard let self = self else { return }
                self.totalRecords = count
                self.isScanning = false
                self.scanComplete = true
                self.isMonitoring = self.bridge.isMonitoring

                if !self.searchText.isEmpty {
                    self.performSearch(self.searchText)
                } else {
                    self.loadRecentFiles()
                }
            }
        }
    }

    func rebuildIndex() {
        guard !isScanning else { return }
        scanComplete = false
        displayItems = []
        totalMatches = 0
        queryTimeMs = 0

        // Delete cached index files so startIncremental does a full scan
        try? FileManager.default.removeItem(atPath: Self.cachePath)
        try? FileManager.default.removeItem(atPath: Self.walPath)

        startIncremental()
    }

    func onSearchTextChanged() {
        searchTask?.cancel()
        recentTask?.cancel()
        searchGeneration &+= 1
        let text = searchText

        if text.isEmpty {
            totalMatches = 0
            queryTimeMs = 0
            cachedIndices = []
            loadedCount = 0
            isContentSearch = false
            contentResults = []
            if scanComplete {
                loadRecentFiles()
            } else {
                displayItems = []
                showingRecent = false
            }
            return
        }
        showingRecent = false

        let lowerText = text.lowercased()
        if lowerText.hasPrefix("infile:") {
            isContentSearch = true
            displayItems = []
            cachedIndices = []
            loadedCount = 0

            let keyword = String(text.dropFirst(7))
            guard !keyword.isEmpty else {
                contentResults = []
                totalMatches = 0
                queryTimeMs = 0
                return
            }

            searchTask = Task { @MainActor in
                // 300ms debounce for content search (heavier)
                try? await Task.sleep(nanoseconds: 300_000_000)
                guard !Task.isCancelled else { return }
                performContentSearch(keyword)
            }
        } else {
            isContentSearch = false
            contentResults = []

            searchTask = Task { @MainActor in
                // 150ms debounce
                try? await Task.sleep(nanoseconds: 150_000_000)
                guard !Task.isCancelled else { return }
                performSearch(text)
            }
        }
    }

    private func performSearch(_ keyword: String) {
        let bridge = self.bridge
        let maxResults = Self.maxResults
        let pageSize = Self.pageSize
        let gen = searchGeneration
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            let indices = bridge.queryIndices(keyword, maxResults: maxResults)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000
            let totalCount = indices.count

            let firstPageCount = min(totalCount, pageSize)
            var items: [FileItem] = []
            items.reserveCapacity(firstPageCount)
            for i in 0..<firstPageCount {
                let idx = indices[i].uint32Value
                if let r = bridge.record(at: idx) {
                    items.append(FileItem(
                        id: idx, name: r.name, path: r.path,
                        type: r.type, size: r.size, modTime: r.modTime
                    ))
                }
            }

            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.cachedIndices = indices
                self.loadedCount = firstPageCount
                self.displayItems = items
                self.totalMatches = totalCount
                self.queryTimeMs = elapsed
            }
        }
    }

    private func performContentSearch(_ keyword: String) {
        let bridge = self.bridge
        let gen = searchGeneration
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            let results = bridge.queryContent(keyword, maxResults: 200)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000

            var items: [ContentFileItem] = []
            items.reserveCapacity(results.count)
            for r in results {
                items.append(ContentFileItem(
                    fileName: r.fileName,
                    filePath: r.filePath,
                    snippet: r.snippet,
                    matchOffset: r.matchOffset,
                    fileType: r.fileType
                ))
            }

            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.contentResults = items
                self.totalMatches = items.count
                self.queryTimeMs = elapsed
            }
        }
    }

    func loadMore() {
        guard !isLoadingMore else { return }
        guard loadedCount < cachedIndices.count else { return }

        isLoadingMore = true
        let bridge = self.bridge
        let indices = cachedIndices
        let currentLoaded = loadedCount
        let pageSize = Self.pageSize
        let gen = searchGeneration

        Task.detached { [weak self] in
            let nextEnd = min(currentLoaded + pageSize, indices.count)
            var newItems: [FileItem] = []
            newItems.reserveCapacity(nextEnd - currentLoaded)
            for i in currentLoaded..<nextEnd {
                let idx = indices[i].uint32Value
                if let r = bridge.record(at: idx) {
                    newItems.append(FileItem(
                        id: idx, name: r.name, path: r.path,
                        type: r.type, size: r.size, modTime: r.modTime
                    ))
                }
            }

            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.displayItems.append(contentsOf: newItems)
                self.loadedCount = nextEnd
                self.isLoadingMore = false
            }
        }
    }

    private func loadRecentFiles() {
        recentTask?.cancel()
        let bridge = self.bridge
        let gen = searchGeneration
        recentTask = Task.detached { [weak self] in
            let indices = bridge.recentIndices(100)
            var items: [FileItem] = []
            items.reserveCapacity(indices.count)
            for num in indices {
                guard !Task.isCancelled else { return }
                let idx = num.uint32Value
                if let r = bridge.record(at: idx) {
                    items.append(FileItem(
                        id: idx, name: r.name, path: r.path,
                        type: r.type, size: r.size, modTime: r.modTime
                    ))
                }
            }
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.displayItems = items
                self.showingRecent = true
            }
        }
    }

    private func onIndexChanged() {
        // Throttle: if a refresh is already scheduled, just mark pending
        if indexChangeTask != nil {
            indexChangePending = true
            return
        }
        performIndexRefresh()
        // Schedule a cooldown — any calls during this window are coalesced
        indexChangeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: Self.indexChangeThrottleNs)
            guard let self else { return }
            self.indexChangeTask = nil
            if self.indexChangePending {
                self.indexChangePending = false
                self.performIndexRefresh()
            }
        }
    }

    private func performIndexRefresh() {
        totalRecords = bridge.liveRecordCount()
        isMonitoring = bridge.isMonitoring

        if !searchText.isEmpty {
            let bridge = self.bridge
            let keyword = searchText
            let maxResults = Self.maxResults
            let gen = searchGeneration
            Task.detached { [weak self] in
                let indices = bridge.queryIndices(keyword, maxResults: maxResults)
                await MainActor.run { [weak self] in
                    guard let self, self.searchGeneration == gen else { return }
                    self.cachedIndices = indices
                    self.totalMatches = indices.count
                }
            }
        } else if showingRecent {
            loadRecentFiles()
        }
    }

    var hasMoreResults: Bool {
        loadedCount < cachedIndices.count
    }

    var contentKeyword: String {
        let lower = searchText.lowercased()
        if lower.hasPrefix("infile:") {
            return String(searchText.dropFirst(7))
        }
        return ""
    }
}
