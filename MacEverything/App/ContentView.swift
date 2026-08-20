import SwiftUI
import AppKit
import QuickLook
import QuickLookUI

struct ContentView: View {
    @StateObject private var viewModel = SearchViewModel()
    @StateObject private var actions = ResultActions()
    @ObservedObject private var searchOptions = SearchOptions.shared
    @ObservedObject private var settings = AppSettings.shared
    @State private var scrollViewID = 0
    @FocusState private var isSearchFieldFocused: Bool
    @State private var quickLookController: QuickLookController?

    var body: some View {
        VStack(spacing: 0) {
            PermissionView()
            searchHeader
            categoryBar
            statusBar
            resultsArea
            actionBar
        }
        .frame(minWidth: 640, minHeight: 430)
        .environment(\.locale, settings.language.locale)
        .onReceive(NotificationCenter.default.publisher(for: .rebuildIndex)) { _ in viewModel.rebuildIndex() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(true)
            isSearchFieldFocused = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(false)
        }
        .onAppear {
            let controller = QuickLookController { [weak actions] in
                guard let item = actions?.selected else { return nil }
                return URL(fileURLWithPath: item.path).appendingPathComponent(item.name)
            }
            controller.install()
            quickLookController = controller
        }
        .onDisappear { quickLookController?.uninstall() }
        .alert("", isPresented: Binding(get: { actions.errorMessage != nil }, set: { if !$0 { actions.errorMessage = nil } })) {
            Button("OK") { actions.errorMessage = nil }
        } message: {
            Text(actions.errorMessage ?? "")
        }
    }

    private var searchHeader: some View {
        HStack(spacing: 12) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 21, weight: .semibold))
                .foregroundStyle(.tint)
            HighlightedSearchField(
                text: $viewModel.searchText,
                placeholder: AppText.value("search.placeholder", language: settings.language),
                ghostSuggestion: viewModel.ghostSuggestion,
                isFocused: $isSearchFieldFocused,
                onTab: {
                    if viewModel.ghostSuggestion != nil { viewModel.acceptGhostSuggestion(); return true }
                    return false
                }
            )
            .frame(height: 34)
            .onChange(of: viewModel.searchText) { viewModel.onSearchTextChanged() }
            SearchOptionBadges(options: searchOptions)
            if !viewModel.searchText.isEmpty {
                Button { viewModel.searchText = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .accessibilityIdentifier("clearButton")
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 11)
        .macEverythingGlass()
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .padding(10)
    }

    private var categoryBar: some View {
        HStack(spacing: 4) {
            ForEach(SearchCategory.allCases) { category in
                let count = viewModel.categoryCounts[category] ?? 0
                let disabled = viewModel.isContentSearch && category != .files
                Button {
                    viewModel.selectedCategory = category
                } label: {
                    HStack(spacing: 5) {
                        Text(AppText.value(category.titleKey, language: settings.language))
                        Text("\(count)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(viewModel.selectedCategory == category ? Color.accentColor.opacity(0.18) : .clear)
                    .clipShape(RoundedRectangle(cornerRadius: 7))
                }
                .buttonStyle(.plain)
                .disabled(disabled)
                .opacity(disabled ? 0.42 : 1)
                .accessibilityIdentifier("category-\(category.rawValue)")
            }
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 7)
    }

    private var statusBar: some View {
        HStack(spacing: 7) {
            if viewModel.isScanning {
                ProgressView().controlSize(.small)
                Text("\(AppText.value("status.scanning", language: settings.language))… \(viewModel.scannedCount)")
            } else if viewModel.scanComplete {
                Circle().fill(.green).frame(width: 6, height: 6)
                Text(AppText.value("status.live", language: settings.language))
                    .foregroundStyle(.green)
                Text("\(viewModel.totalRecords) \(AppText.value("status.indexed", language: settings.language))")
                    .foregroundStyle(.secondary)
                if viewModel.totalMatches > 0 {
                    Text("·").foregroundStyle(.secondary)
                    Text("\(viewModel.totalMatches) \(AppText.value("status.matches", language: settings.language))")
                        .foregroundStyle(.secondary)
                    Text(String(format: "%.1fms", viewModel.queryTimeMs))
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
        .font(.callout)
        .padding(.horizontal, 14)
        .padding(.vertical, 6)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.65))
    }

    @ViewBuilder
    private var resultsArea: some View {
        if viewModel.isScanning {
            VStack(spacing: 10) {
                Spacer()
                ProgressView().controlSize(.large)
                Text("\(AppText.value("status.scanning", language: settings.language))… \(viewModel.scannedCount)")
                    .foregroundStyle(.secondary)
                Spacer()
            }
        } else if viewModel.isContentSearch {
            if viewModel.contentKeyword.isEmpty {
                emptyState(AppText.value("empty.start", language: settings.language))
            } else if viewModel.contentResults.isEmpty {
                emptyState(AppText.value("empty.content", language: settings.language))
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(viewModel.contentResults) { item in
                            let selected = actions.selected?.id == contentItem(item).id
                            ContentResultRow(item: item, keyword: viewModel.contentKeyword,
                                             isSelected: selected,
                                             onSelect: { actions.select(contentItem(item)) },
                                             onOpen: { actions.select(contentItem(item)); actions.openSelected() },
                                             onReveal: { actions.select(contentItem(item)); actions.revealSelected() })
                                .padding(.horizontal, 8).padding(.vertical, 2)
                        }
                    }
                }
                .id(scrollViewID)
                .accessibilityIdentifier("contentResultsList")
            }
        } else if viewModel.searchText.isEmpty {
            emptyState(AppText.value("empty.start", language: settings.language))
        } else if viewModel.displayItems.isEmpty && viewModel.scanComplete {
            emptyState(AppText.value("empty.noResults", language: settings.language))
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(viewModel.displayItems) { item in
                        ResultRow(item: item, hints: viewModel.highlightHints,
                                  isSelected: actions.selected?.id == item.id,
                                  onSelect: { actions.select(item) },
                                  onOpen: { actions.select(item); actions.openSelected() },
                                  onReveal: { actions.select(item); actions.revealSelected() })
                            .padding(.horizontal, 8).padding(.vertical, 2)
                            .id(item.id)
                    }
                    if viewModel.hasMoreResults {
                        ProgressView().controlSize(.small).padding(8).onAppear { viewModel.loadMore() }
                    }
                }
            }
            .id(scrollViewID)
            .accessibilityIdentifier("fileResultsList")
        }
    }

    private func emptyState(_ text: String) -> some View {
        VStack { Spacer(); Text(text).foregroundStyle(.secondary); Spacer() }
    }

    private var actionBar: some View {
        HStack(spacing: 8) {
            Spacer()
            actionButton("action.open", systemImage: "arrow.up.forward.app", enabled: actions.selected != nil) { actions.openSelected() }
            actionButton("action.reveal", systemImage: "folder", enabled: actions.selected != nil) { actions.revealSelected() }
            actionButton("action.delete", systemImage: "trash", enabled: actions.selected != nil) { actions.deleteSelected() }
            actionButton("action.undo", systemImage: "arrow.uturn.backward", enabled: actions.canUndo) { actions.undoDelete() }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
    }

    private func actionButton(_ key: String, systemImage: String, enabled: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(AppText.value(key, language: settings.language), systemImage: systemImage)
        }
        .buttonStyle(.bordered)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.45)
    }

    private func contentItem(_ item: ContentFileItem) -> FileItem {
        let url = URL(fileURLWithPath: item.filePath)
        return FileItem(id: item.filePath, index: 0, name: url.lastPathComponent,
                        path: url.deletingLastPathComponent().path, type: item.fileType,
                        size: 0, modTime: 0)
    }
}

final class QuickLookController: NSObject, QLPreviewPanelDataSource {
    private let urlProvider: () -> URL?
    private var monitor: Any?

    init(urlProvider: @escaping () -> URL?) { self.urlProvider = urlProvider }

    func install() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard event.keyCode == 49,
                  !(event.window?.firstResponder is NSTextView),
                  let self, let url = self.urlProvider() else { return event }
            QLPreviewPanel.shared().dataSource = self
            QLPreviewPanel.shared().reloadData()
            QLPreviewPanel.shared().makeKeyAndOrderFront(nil)
            _ = url
            return nil
        }
    }

    func uninstall() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
    }

    func numberOfPreviewItems(in panel: QLPreviewPanel) -> Int { urlProvider() == nil ? 0 : 1 }
    func previewPanel(_ panel: QLPreviewPanel, previewItemAt index: Int) -> QLPreviewItem {
        urlProvider()! as NSURL
    }
}
