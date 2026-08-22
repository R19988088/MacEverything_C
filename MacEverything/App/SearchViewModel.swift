import Foundation
import Combine

struct FileItem: Identifiable, Equatable {
    let id: String      // path-based stable ID
    let index: UInt32   // engine index for record lookup
    let name: String
    let path: String
    let type: UInt8
    let size: UInt64
    let modTime: time_t
}

struct ContentFileItem: Identifiable, Equatable {
    let id: String      // stable ID: filePath + ":" + matchOffset
    let fileName: String
    let filePath: String
    let snippet: String
    let matchOffset: UInt32
    let fileType: UInt8
}

enum SearchCategory: Int, CaseIterable, Identifiable {
    case applications = 0, files, folders, images, videos, audio, archives, brushes

    var id: Int { rawValue }
    var titleKey: String {
        switch self {
        case .applications: return "category.applications"
        case .files: return "category.files"
        case .folders: return "category.folders"
        case .images: return "category.images"
        case .videos: return "category.videos"
        case .audio: return "category.audio"
        case .archives: return "category.archives"
        case .brushes: return "category.brushes"
        }
    }
}

enum ResultSortField: String {
    case name, path, size, modified
}

enum ResultSortDirection: String {
    case ascending, descending
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
    var isLoadingMore: Bool = false
    @Published var showingRecent: Bool = false
    @Published var selectedCategory: SearchCategory = .files {
        didSet {
            appSettings.selectedCategoryRawValue = selectedCategory.rawValue
            updateDisplayedCategory()
        }
    }
    @Published private(set) var categoryCounts: [SearchCategory: Int] = Dictionary(
        uniqueKeysWithValues: SearchCategory.allCases.map { ($0, 0) }
    )
    @Published private(set) var resultSortField: ResultSortField?
    @Published private(set) var resultSortDirection: ResultSortDirection = .ascending
    @Published var isContentSearch: Bool = false
    @Published var contentResults: [ContentFileItem] = []
    @Published var isContentIndexing: Bool = false
    @Published var contentIndexProgress: (indexed: UInt32, total: UInt32)?
    @Published var contentIndexedCount: UInt32 = 0
    @Published var isSyncing: Bool = false
    @Published var ghostSuggestion: String? = nil

    /// Structured highlight hints extracted from the C++ query AST.
    /// Replaces the old keyword-based approach with field-aware, mode-aware hints.
    var highlightHints: [HighlightHint] {
        let query = searchOptions.buildQuery(searchText)
        guard !query.isEmpty else { return [] }
        return bridge.parseHighlightHints(query).map { HighlightHint(from: $0) }
    }

    private let bridge = MacSearchBridge.shared()
    private let historyStore = SearchHistoryStore()
    private let appSettings = AppSettings.shared
    private let searchOptions = SearchOptions.shared
    private var searchTask: Task<Void, Never>?
    private var recentTask: Task<Void, Never>?
    private var settledTask: Task<Void, Never>?
    private var optionsSink: AnyCancellable?
    private var exclusionsSink: AnyCancellable?
    private var enabledCategoriesSink: AnyCancellable?
    private var cachedResults: [MEFileResult] = []
    private var loadedCount: Int = 0
    private var searchGeneration: UInt64 = 0
    private static let guiSessionId: UInt64 = 1

    private static let pageSize: Int = 100
    private static let maxResults: UInt32 = 10000
    private static let indexChangeThrottleNs: UInt64 = 500_000_000

    private var categoryResults: [SearchCategory: [MEFileResult]] = Dictionary(
        uniqueKeysWithValues: SearchCategory.allCases.map { ($0, []) }
    )

    private var indexChangeTask: Task<Void, Never>?
    let refreshThrottle = IndexRefreshThrottle()

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

    static var pagesPath: String {
        return (cacheDir as NSString).appendingPathComponent("index.pages")
    }

    static var ptablePath: String {
        return (cacheDir as NSString).appendingPathComponent("index.ptable")
    }

    init() {
        selectedCategory = SearchCategory(rawValue: appSettings.selectedCategoryRawValue) ?? .files
        if let launchQuery = appSettings.launchQuery {
            searchText = launchQuery
        }
        optionsSink = searchOptions.objectWillChange.sink { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.onSearchOptionsChanged()
            }
        }
        exclusionsSink = appSettings.$excludedFolders.sink { [weak self] _ in
            Task { @MainActor [weak self] in self?.refreshExcludedResults() }
        }
        enabledCategoriesSink = appSettings.$enabledCategoryRawValues.sink { [weak self] _ in
            Task { @MainActor [weak self] in self?.updateDisplayedCategory() }
        }
        startIncremental()
    }

    private func onSearchOptionsChanged() {
        guard scanComplete, !searchText.isEmpty, !isContentSearch else { return }
        searchTask?.cancel()
        searchGeneration &+= 1
        bridge.cancelSession(Self.guiSessionId)
        performSearch(searchText)
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
                self.contentIndexedCount = totalIndexed
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
                self.isSyncing = self.bridge.isSyncing

                if !self.searchText.isEmpty {
                    self.performSearch(self.searchText)
                }
            }
        }
    }

    func rebuildIndex() {
        guard !isScanning else { return }
        searchTask?.cancel()
        recentTask?.cancel()
        indexChangeTask?.cancel()
        searchGeneration &+= 1
        scanComplete = false
        displayItems = []
        totalMatches = 0
        categoryCounts = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, 0) })
        categoryResults = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, []) })
        queryTimeMs = 0
        contentResults = []
        isContentSearch = false
        contentKeyword = ""

        // Delete cached index files so startIncremental does a full scan
        try? FileManager.default.removeItem(atPath: Self.cachePath)
        try? FileManager.default.removeItem(atPath: Self.walPath)
        try? FileManager.default.removeItem(atPath: Self.pagesPath)
        try? FileManager.default.removeItem(atPath: Self.ptablePath)

        startIncremental()
    }

    func onSearchTextChanged() {
        searchTask?.cancel()
        recentTask?.cancel()
        searchGeneration &+= 1
        isLoadingMore = false
        let text = searchText

        if text.isEmpty {
            totalMatches = 0
            queryTimeMs = 0
            cachedResults = []
            loadedCount = 0
            isContentSearch = false
            contentResults = []
            contentKeyword = "" // H-9: reset cached keyword
            ghostSuggestion = nil
            settledTask?.cancel()
            // Cancel any in-flight queries for this GUI session
            bridge.cancelSession(Self.guiSessionId)
            displayItems = []
            showingRecent = false
            categoryCounts = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, 0) })
            categoryResults = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, []) })
            return
        }
        showingRecent = false

        let lowerText = text.lowercased()
        if lowerText.hasPrefix("infile:") {
            isContentSearch = true
            displayItems = []
            cachedResults = []
            loadedCount = 0

            let keyword = String(text.dropFirst(7))
            contentKeyword = keyword // H-9: cache computed keyword
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
            contentKeyword = "" // H-9: reset cached keyword

            searchTask = Task { @MainActor in
                // 80ms debounce
                try? await Task.sleep(nanoseconds: 80_000_000)
                guard !Task.isCancelled else { return }
                performSearch(text)
            }
        }

        updateGhostSuggestion()
        scheduleHistoryRecord()
    }

    private func performSearch(_ keyword: String) {
        let bridge = self.bridge
        let maxResults = Self.maxResults
        let gen = searchGeneration
        let query = searchOptions.buildQuery(keyword)
        Task.detached { [weak self] in
            let start = CFAbsoluteTimeGetCurrent()
            let facets = bridge.queryFacetedResults(query, maxResultsPerCategory: maxResults,
                                                     sessionId: Self.guiSessionId)
            let elapsed = (CFAbsoluteTimeGetCurrent() - start) * 1000
            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else { return }
                self.categoryResults = Self.resultDictionary(from: facets)
                self.categoryCounts = Self.countDictionary(from: facets)
                self.updateDisplayedCategory()
                self.queryTimeMs = elapsed
                self.showingRecent = false
                self.appSettings.recordSuccessfulQuery(keyword)
            }
        }
    }

    private static func resultDictionary(from facets: MEFacetQueryResult) -> [SearchCategory: [MEFileResult]] {
        Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, results(for: $0, in: facets)) })
    }

    private static func countDictionary(from facets: MEFacetQueryResult) -> [SearchCategory: Int] {
        Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, count(for: $0, in: facets)) })
    }

    private static func results(for category: SearchCategory, in facets: MEFacetQueryResult) -> [MEFileResult] {
        switch category {
        case .applications: return facets.applications
        case .files: return facets.files
        case .folders: return facets.folders
        case .images: return facets.images
        case .videos: return facets.videos
        case .audio: return facets.audio
        case .archives: return facets.archives
        case .brushes: return facets.brushes
        }
    }

    private static func count(for category: SearchCategory, in facets: MEFacetQueryResult) -> Int {
        switch category {
        case .applications: return Int(facets.applicationsCount)
        case .files: return Int(facets.filesCount)
        case .folders: return Int(facets.foldersCount)
        case .images: return Int(facets.imagesCount)
        case .videos: return Int(facets.videosCount)
        case .audio: return Int(facets.audioCount)
        case .archives: return Int(facets.archivesCount)
        case .brushes: return Int(facets.brushesCount)
        }
    }

    private func updateDisplayedCategory() {
        guard !isContentSearch else { return }
        if !appSettings.isCategoryEnabled(selectedCategory),
           let fallback = SearchCategory.allCases.first(where: appSettings.isCategoryEnabled) {
            selectedCategory = fallback
            return
        }
        let filteredByCategory = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { category in
            (category, categoryResults[category, default: []].filter { !isExcluded($0) })
        })
        let results = sortedResults(filteredByCategory[selectedCategory] ?? [])
        cachedResults = results
        loadedCount = min(results.count, Self.pageSize)
        displayItems = results.prefix(loadedCount).map(makeItem)
        categoryCounts = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { category in
            (category, filteredByCategory[category]?.count ?? 0)
        })
        totalMatches = filteredByCategory[selectedCategory]?.count ?? 0
    }

    private func refreshExcludedResults() {
        guard !isContentSearch else { return }
        updateDisplayedCategory()
    }

    private func isExcluded(_ result: MEFileResult) -> Bool {
        let fullPath = URL(fileURLWithPath: result.path)
            .appendingPathComponent(result.name).standardizedFileURL.path
        return appSettings.excludedFolders.contains { folder in
            fullPath == folder || fullPath.hasPrefix(folder + "/") || result.path == folder || result.path.hasPrefix(folder + "/")
        }
    }

    func excludeFolder(for item: FileItem) {
        let folder = item.type == 2
            ? URL(fileURLWithPath: item.path).appendingPathComponent(item.name).path
            : item.path
        appSettings.excludeFolder(folder)
    }

    func toggleResultSort(_ field: ResultSortField) {
        if resultSortField == field {
            resultSortDirection = resultSortDirection == .ascending ? .descending : .ascending
        } else {
            resultSortField = field
            resultSortDirection = .ascending
        }
        updateDisplayedCategory()
    }

    private func sortedResults(_ results: [MEFileResult]) -> [MEFileResult] {
        guard let field = resultSortField else { return results }
        let direction = resultSortDirection
        return results.sorted { lhs, rhs in
            let comparison = Self.compare(lhs, rhs, by: field)
            return direction == .ascending ? comparison == .orderedAscending : comparison == .orderedDescending
        }
    }

    private static func compare(_ lhs: MEFileResult, _ rhs: MEFileResult,
                                by field: ResultSortField) -> ComparisonResult {
        let primary: ComparisonResult
        switch field {
        case .name:
            primary = lhs.name.localizedCaseInsensitiveCompare(rhs.name)
        case .path:
            primary = lhs.path.localizedCaseInsensitiveCompare(rhs.path)
        case .size:
            primary = lhs.size == rhs.size ? .orderedSame : (lhs.size < rhs.size ? .orderedAscending : .orderedDescending)
        case .modified:
            primary = lhs.modTime == rhs.modTime ? .orderedSame : (lhs.modTime < rhs.modTime ? .orderedAscending : .orderedDescending)
        }
        guard primary == .orderedSame else { return primary }

        let name = lhs.name.localizedCaseInsensitiveCompare(rhs.name)
        if name != .orderedSame { return name }
        return lhs.path.localizedCaseInsensitiveCompare(rhs.path)
    }

    private func makeItem(_ result: MEFileResult) -> FileItem {
        FileItem(id: "\(result.path)/\(result.name)", index: 0, name: result.name,
                 path: result.path, type: result.type, size: result.size, modTime: result.modTime)
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
                    id: "\(r.filePath):\(r.matchOffset)",
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
        self.categoryCounts = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map {
            ($0, $0 == .files ? items.count : 0)
        })
        self.categoryResults = Dictionary(uniqueKeysWithValues: SearchCategory.allCases.map { ($0, []) })
        self.categoryResults[.files] = []
                self.queryTimeMs = elapsed
            }
        }
    }

    func loadMore() {
        guard !isLoadingMore else { return }
        guard loadedCount < cachedResults.count else { return }

        isLoadingMore = true
        let results = cachedResults
        let currentLoaded = loadedCount
        let pageSize = Self.pageSize
        let gen = searchGeneration

        // P-4: Results are already fetched — just slice the next page, no bridge calls needed
        Task.detached { [weak self] in
            let nextEnd = min(currentLoaded + pageSize, results.count)
            var newItems: [FileItem] = []
            newItems.reserveCapacity(nextEnd - currentLoaded)
            for i in currentLoaded..<nextEnd {
                let r = results[i]
                newItems.append(FileItem(
                    id: "\(r.path)/\(r.name)", index: 0,
                    name: r.name, path: r.path,
                    type: r.type, size: r.size, modTime: r.modTime
                ))
            }

            await MainActor.run { [weak self] in
                guard let self, self.searchGeneration == gen else {
                    self?.isLoadingMore = false
                    return
                }
                self.displayItems.append(contentsOf: newItems)
                self.loadedCount = nextEnd
                self.isLoadingMore = false
            }
        }
    }

    private func loadRecentFiles() {
        displayItems = []
        showingRecent = false
    }

    private func onIndexChanged() {
        if refreshThrottle.indexChanged() {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    func onWindowFocusChanged(_ focused: Bool) {
        if !focused {
            // Record search text to history when window loses focus
            let text = searchText
            if text.count >= 2 && !text.lowercased().hasPrefix("infile:") {
                historyStore.recordQuery(text)
            }
        }
        if refreshThrottle.focusChanged(focused) {
            performIndexRefresh()
            scheduleCooldown()
        }
    }

    private func scheduleCooldown() {
        indexChangeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: Self.indexChangeThrottleNs)
            guard let self else { return }
            self.indexChangeTask = nil
            if self.refreshThrottle.cooldownExpired() {
                self.performIndexRefresh()
                self.scheduleCooldown()
            }
        }
    }

    private func performIndexRefresh() {
        totalRecords = bridge.liveRecordCount()
        isMonitoring = bridge.isMonitoring
        isSyncing = bridge.isSyncing
        contentIndexedCount = bridge.contentIndexedFileCount()

        // Skip expensive search/query when app is not focused.
        // Results will refresh on focus regain via onWindowFocusChanged.
        guard refreshThrottle.isFocused else { return }

        if !searchText.isEmpty && !isContentSearch {
            performSearch(searchText)
        } else if isContentSearch && !contentKeyword.isEmpty {
            performContentSearch(contentKeyword)
        } else if searchText.isEmpty {
            displayItems = []
        }
    }

    var hasMoreResults: Bool {
        loadedCount < cachedResults.count
    }

    // H-9: Cached to avoid recomputing lowercased() + hasPrefix on every access
    private(set) var contentKeyword: String = ""

    // MARK: - Ghost text autocomplete

    private static let systemKeywords: [String] = [
        // Basic filters (most common first)
        "ext:", "size:", "file:", "folder:", "path:", "nopath:", "parent:", "depth:", "len:",
        // Date filters
        "dm:", "dc:", "da:", "datemodified:", "datecreated:", "dateaccessed:",
        // Modifiers
        "case:", "nocase:", "regex:", "ww:", "wfn:", "wholeword:", "wholefilename:",
        // Macros
        "audio:", "video:", "pic:", "doc:", "exe:", "zip:",
        // Other
        "content:", "type:",
    ]

    private static func systemKeywordMatch(for text: String) -> String? {
        let lower = text.lowercased()
        // Only match single partial words: no spaces, no colon already present
        guard !lower.isEmpty, !lower.contains(" "), !lower.contains(":") else { return nil }

        let matches = systemKeywords.filter { $0.hasPrefix(lower) }
        // Prefer shorter keywords, then alphabetical
        return matches.min { a, b in
            if a.count != b.count { return a.count < b.count }
            return a < b
        }
    }

    private func updateGhostSuggestion() {
        let text = searchText
        guard !text.isEmpty, !text.lowercased().hasPrefix("infile:") else {
            ghostSuggestion = nil
            return
        }
        // Priority 1: search history match
        if let match = historyStore.bestMatch(for: text),
           match.lowercased() != text.lowercased() {
            ghostSuggestion = text + match.dropFirst(text.count)
            return
        }
        // Priority 2: system keyword match
        if let keyword = Self.systemKeywordMatch(for: text) {
            ghostSuggestion = text + keyword.dropFirst(text.count)
            return
        }
        ghostSuggestion = nil
    }

    private func scheduleHistoryRecord() {
        settledTask?.cancel()
        let text = searchText
        guard text.count >= 2, !text.lowercased().hasPrefix("infile:") else { return }
        settledTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000) // 2 seconds
            guard !Task.isCancelled, let self, self.searchText == text else { return }
            self.historyStore.recordQuery(text)
        }
    }

    func acceptGhostSuggestion() {
        guard let suggestion = ghostSuggestion else { return }
        searchText = suggestion
        ghostSuggestion = nil
        historyStore.recordQuery(suggestion)
    }
}
