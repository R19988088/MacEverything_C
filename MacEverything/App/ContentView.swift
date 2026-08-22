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
    @FocusState private var isResultsFocused: Bool
    @State private var quickLookController: QuickLookController?
    @State private var copyController: SelectionCopyController?
    @State private var navigationController: ResultNavigationController?

    var body: some View {
        VStack(spacing: 0) {
            customTitleBar
            PermissionView()
            searchHeader
            categoryBar
            resultsArea
            actionBar
        }
        .modifier(WindowMaterialBackground())
        .ignoresSafeArea(.container, edges: .top)
        .frame(minWidth: 640, minHeight: 430)
        .tint(Color(hex: settings.themeColorHex))
        .accentColor(Color(hex: settings.themeColorHex))
        .environment(\.locale, settings.language.locale)
        .onReceive(NotificationCenter.default.publisher(for: .rebuildIndex)) { _ in viewModel.rebuildIndex() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(true)
            isSearchFieldFocused = true
            actions.isSearchFieldFocused = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)) { _ in
            viewModel.onWindowFocusChanged(false)
        }
        .onChange(of: isSearchFieldFocused) { actions.isSearchFieldFocused = isSearchFieldFocused }
        .onAppear {
            let controller = QuickLookController(
                itemsProvider: { [weak viewModel] in viewModel?.displayItems ?? [] },
                selectedIDProvider: { [weak actions] in actions?.selected?.id },
                onNavigate: { [weak actions] item in actions?.select(item) },
                canPreview: { [weak actions] in
                    guard let actions else { return false }
                    return actions.selected != nil && !actions.isSearchFieldFocused
                })
            controller.install()
            quickLookController = controller
            let navigation = ResultNavigationController { [weak viewModel, weak actions] direction in
                guard let viewModel, let actions, !actions.isSearchFieldFocused,
                      !viewModel.isContentSearch else { return }
                let items = viewModel.displayItems
                guard !items.isEmpty else { return }
                let current = actions.selected.flatMap { selected in items.firstIndex(where: { $0.id == selected.id }) } ?? (direction > 0 ? -1 : items.count)
                let next = min(max(current + direction, 0), items.count - 1)
                guard next != current else { return }
                actions.select(items[next])
            }
            navigation.install()
            navigationController = navigation
            let copyController = SelectionCopyController { [weak actions] in actions?.copySelected() }
            copyController.install()
            self.copyController = copyController
        }
        .onDisappear {
            quickLookController?.uninstall()
            copyController?.uninstall()
            navigationController?.uninstall()
        }
        .onChange(of: actions.selected?.id) { quickLookController?.selectionDidChange() }
        .alert("", isPresented: Binding(get: { actions.errorMessage != nil }, set: { if !$0 { actions.errorMessage = nil } })) {
            Button("OK") { actions.errorMessage = nil }
        } message: {
            Text(actions.errorMessage ?? "")
        }
    }

    private var customTitleBar: some View {
        ZStack {
            Text("maceverything")
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .frame(height: 38)
        .contentShape(Rectangle())
    }

    private var searchHeader: some View {
        HStack(spacing: 12) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 21, weight: .semibold))
                .foregroundStyle(.tint)
            ZStack(alignment: .leading) {
                if let ghost = viewModel.ghostSuggestion, !ghost.isEmpty {
                    Text(ghost)
                        .font(.system(size: 26))
                        .foregroundStyle(.secondary.opacity(0.4))
                        .lineLimit(1)
                        .allowsHitTesting(false)
                }
                TextField(
                    AppText.value("search.placeholder", language: settings.language),
                    text: $viewModel.searchText
                )
                .focused($isSearchFieldFocused)
                .textFieldStyle(.plain)
                .font(.system(size: 26))
                .accessibilityIdentifier("searchField")
                .onChange(of: viewModel.searchText) { viewModel.onSearchTextChanged() }
                .onKeyPress(.tab) {
                    if viewModel.ghostSuggestion != nil {
                        viewModel.acceptGhostSuggestion()
                        return .handled
                    }
                    return .ignored
                }
            }
            .frame(height: 34)
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
        .clipShape(Capsule())
        .overlay(Capsule().stroke(Color.secondary.opacity(0.18), lineWidth: 1))
        .padding(10)
    }

    @ViewBuilder
    private var categoryBar: some View {
        HStack(spacing: 4) {
            ForEach(SearchCategory.allCases) { category in
                let count = viewModel.categoryCounts[category] ?? 0
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
                    .background(viewModel.selectedCategory == category ? Color(hex: settings.themeColorHex).opacity(0.18) : .clear)
                    .clipShape(RoundedRectangle(cornerRadius: 7))
                }
                .buttonStyle(.plain)
                .disabled((viewModel.isContentSearch && category != .files) || !settings.isCategoryEnabled(category))
                .opacity(settings.isCategoryEnabled(category) ? 1 : 0.4)
                .accessibilityIdentifier("category-\(category.titleKey.replacingOccurrences(of: "category.", with: ""))")
            }
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 7)
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
                            ContentResultRow(item: item, keyword: viewModel.contentKeyword, iconScale: settings.iconScale,
                                             isSelected: selected,
                                             onSelect: { select(contentItem(item)) },
                                             onOpen: { select(contentItem(item)); actions.openSelected() },
                                             onReveal: { select(contentItem(item)); actions.revealSelected() })
                                .equatable()
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
            let highlightHints = viewModel.highlightHints
            VStack(spacing: 0) {
                resultHeader
                Divider()
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(viewModel.displayItems) { item in
                            ResultRow(item: item, hints: highlightHints, iconScale: settings.iconScale,
                                      isSelected: actions.selected?.id == item.id,
                                      onSelect: { select(item) },
                                      onOpen: { select(item); actions.openSelected() },
                                      onReveal: { select(item); actions.revealSelected() },
                                      onExcludeFolder: { viewModel.excludeFolder(for: item) })
                                .equatable()
                                .overlay(alignment: .bottom) { Divider().opacity(0.45) }
                        }
                        if viewModel.hasMoreResults {
                            ProgressView().controlSize(.small).padding(8).onAppear { viewModel.loadMore() }
                        }
                    }
                }
                .id(scrollViewID)
                .accessibilityIdentifier("fileResultsList")
                .focusable(true)
                .focusEffectDisabled()
                .focused($isResultsFocused)
                .onMoveCommand { direction in
                    moveSelection(direction)
                }
            }
            .padding(8)
            .background(Color(nsColor: .textBackgroundColor).opacity(0.52))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.secondary.opacity(0.18), lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
        }
    }

    private func emptyState(_ text: String) -> some View {
        VStack { Spacer(); Text(text).foregroundStyle(.secondary); Spacer() }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(8)
            .background(Color(nsColor: .textBackgroundColor).opacity(0.52))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.secondary.opacity(0.18), lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
    }

    private var actionBar: some View {
        HStack(spacing: 8) {
            actionButton("action.open", systemImage: "arrow.up.forward.app", enabled: actions.selected != nil) { actions.openSelected() }
            actionButton("action.reveal", systemImage: "folder", enabled: actions.selected != nil) { actions.revealSelected() }
            actionButton("action.delete", systemImage: "trash", enabled: actions.selected != nil) { actions.deleteSelected() }
            actionButton("action.undo", systemImage: "arrow.uturn.backward", enabled: actions.canUndo) { actions.undoDelete() }
            Spacer(minLength: 16)
            iconScaleControl
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.clear)
    }

    private var iconScaleControl: some View {
        HStack(spacing: 7) {
            Image(systemName: "square.grid.3x3")
                .font(.system(size: 10))
            Slider(value: $settings.iconScale, in: 1...4)
                .frame(width: 150)
                .accessibilityIdentifier("iconScaleSlider")
            Image(systemName: "square.grid.3x3")
                .font(.system(size: 18))
        }
        .foregroundStyle(.secondary)
    }

    private func actionButton(_ key: String, systemImage: String, enabled: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(AppText.value(key, language: settings.language), systemImage: systemImage)
        }
        .buttonStyle(.bordered)
        .controlSize(.regular)
        .frame(minHeight: 36)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.45)
    }

    private var resultHeader: some View {
        HStack(spacing: 0) {
            sortableHeader("result.name", field: .name, minWidth: 220, alignment: .leading)
            headerDivider
            sortableHeader("result.path", field: .path, minWidth: 180, alignment: .leading)
            headerDivider
            sortableHeader("result.size", field: .size, width: 88, alignment: .leading)
            headerDivider
            sortableHeader("result.modified", field: .modified, width: 168, alignment: .leading)
        }
        .font(.caption.weight(.semibold))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }

    private func sortableHeader(_ key: String, field: ResultSortField,
                                minWidth: CGFloat = 0, width: CGFloat? = nil,
                                alignment: Alignment) -> some View {
        Button { viewModel.toggleResultSort(field) } label: {
            HStack(spacing: 4) {
                if alignment == .trailing { Spacer(minLength: 0) }
                Text(AppText.value(key, language: settings.language))
                if viewModel.resultSortField == field {
                    Image(systemName: viewModel.resultSortDirection == .ascending ? "chevron.up" : "chevron.down")
                        .font(.system(size: 8, weight: .bold))
                }
                if alignment == .leading { Spacer(minLength: 0) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .frame(minWidth: width ?? minWidth, maxWidth: width ?? .infinity, alignment: alignment)
        .accessibilityIdentifier("sort-\(field.rawValue)")
        .accessibilityValue(viewModel.resultSortField == field ? viewModel.resultSortDirection.rawValue : "none")
    }

    private var headerDivider: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.18))
            .frame(width: 1, height: 16)
            .padding(.horizontal, 6)
    }

    private func select(_ item: FileItem) {
        isSearchFieldFocused = false
        isResultsFocused = true
        actions.isSearchFieldFocused = false
        actions.select(item)
    }

    private func moveSelection(_ direction: MoveCommandDirection) {
        guard !viewModel.displayItems.isEmpty else { return }
        let delta = direction == .down ? 1 : -1
        let current = actions.selected.flatMap { selected in
            viewModel.displayItems.firstIndex(where: { $0.id == selected.id })
        } ?? (delta > 0 ? -1 : viewModel.displayItems.count)
        let next = min(max(current + delta, 0), viewModel.displayItems.count - 1)
        guard next != current else { return }
        select(viewModel.displayItems[next])
    }

    private func contentItem(_ item: ContentFileItem) -> FileItem {
        let url = URL(fileURLWithPath: item.filePath)
        return FileItem(id: item.filePath, index: 0, name: url.lastPathComponent,
                        path: url.deletingLastPathComponent().path, type: item.fileType,
                        size: 0, modTime: 0)
    }
}

private struct WindowMaterialBackground: ViewModifier {
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content.glassEffect(.regular, in: RoundedRectangle(cornerRadius: 22))
        } else {
            content.background(.ultraThinMaterial)
        }
    }
}

final class QuickLookController: NSObject, QLPreviewPanelDataSource {
    private let itemsProvider: () -> [FileItem]
    private let selectedIDProvider: () -> String?
    private let onNavigate: (FileItem) -> Void
    private let canPreview: () -> Bool
    private var monitor: Any?
    private var navigationMonitor: Any?

    init(itemsProvider: @escaping () -> [FileItem], selectedIDProvider: @escaping () -> String?, onNavigate: @escaping (FileItem) -> Void, canPreview: @escaping () -> Bool) {
        self.itemsProvider = itemsProvider
        self.selectedIDProvider = selectedIDProvider
        self.onNavigate = onNavigate
        self.canPreview = canPreview
    }

    func install() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            if let self, self.handleNavigation(event) { return nil }
            guard event.keyCode == 49,
                  event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty,
                  let self, self.canPreview(), !self.itemsProvider().isEmpty else { return event }
            if QLPreviewPanel.shared().isVisible {
                QLPreviewPanel.shared().orderOut(nil)
                return nil
            }
            QLPreviewPanel.shared().dataSource = self
            QLPreviewPanel.shared().reloadData()
            let items = self.itemsProvider()
            if let selectedID = self.selectedIDProvider(),
               let index = items.firstIndex(where: { $0.id == selectedID }) {
                QLPreviewPanel.shared().currentPreviewItemIndex = index
            }
            QLPreviewPanel.shared().makeKeyAndOrderFront(nil)
            return nil
        }
    }

    func uninstall() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        if let navigationMonitor { NSEvent.removeMonitor(navigationMonitor) }
        navigationMonitor = nil
    }

    private func handleNavigation(_ event: NSEvent) -> Bool {
        guard QLPreviewPanel.shared().isVisible,
              event.keyCode == 125 || event.keyCode == 126,
              event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty else { return false }
        let items = itemsProvider()
        guard items.count > 1 else { return false }
        guard let panel = QLPreviewPanel.shared() else { return false }
        let current = panel.currentPreviewItemIndex >= 0 ? panel.currentPreviewItemIndex : 0
        let next = event.keyCode == 125
            ? min(current + 1, items.count - 1)
            : max(current - 1, 0)
        guard next != current else { return true }
        onNavigate(items[next])
        return true
    }

    func selectionDidChange() {
        guard let panel = QLPreviewPanel.shared(), panel.isVisible else { return }
        let items = itemsProvider()
        guard let selectedID = selectedIDProvider(),
              let index = items.firstIndex(where: { $0.id == selectedID }) else { return }
        panel.reloadData()
        panel.currentPreviewItemIndex = index
    }

    func numberOfPreviewItems(in panel: QLPreviewPanel) -> Int { itemsProvider().count }
    func previewPanel(_ panel: QLPreviewPanel, previewItemAt index: Int) -> QLPreviewItem {
        let item = itemsProvider()[index]
        return URL(fileURLWithPath: item.path).appendingPathComponent(item.name) as NSURL
    }
}

@MainActor
final class SelectionCopyController {
    private let copy: () -> Void
    private var monitor: Any?

    init(copy: @escaping () -> Void) { self.copy = copy }

    func install() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
            guard event.keyCode == 8, modifiers == .command,
                  !(event.window?.firstResponder is NSTextView),
                  !(event.window?.firstResponder is NSTextField) else { return event }
            self?.copy()
            return nil
        }
    }

    func uninstall() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
    }
}

@MainActor
final class ResultNavigationController {
    private let move: (Int) -> Void
    private var monitor: Any?

    init(move: @escaping (Int) -> Void) { self.move = move }

    func install() {
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self,
                  event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty,
                  event.keyCode == 125 || event.keyCode == 126,
                  !(QLPreviewPanel.shared()?.isVisible ?? false) else { return event }
            self.move(event.keyCode == 125 ? 1 : -1)
            return nil
        }
    }

    func uninstall() {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
    }
}
