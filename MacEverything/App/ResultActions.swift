import AppKit
import SwiftUI

@MainActor
final class ResultActions: ObservableObject {
    @Published private(set) var selected: FileItem?
    @Published private(set) var canUndo = false
    @Published var isSearchFieldFocused = true
    @Published var errorMessage: String?

    private var trashedURL: URL?
    private var originalURL: URL?

    func select(_ item: FileItem?) { selected = item }

    func openSelected() {
        guard let item = selected else { return }
        let url = URL(fileURLWithPath: item.path).appendingPathComponent(item.name)
        if item.type == 5 {
            NSWorkspace.shared.openApplication(at: url, configuration: NSWorkspace.OpenConfiguration())
        } else if !NSWorkspace.shared.open(url) {
            NSSound.beep()
        }
    }

    func revealSelected() {
        guard let item = selected else { return }
        let url = URL(fileURLWithPath: item.path).appendingPathComponent(item.name)
        NSWorkspace.shared.selectFile(url.path, inFileViewerRootedAtPath: "")
    }

    func deleteSelected() {
        guard let item = selected else { return }
        let source = URL(fileURLWithPath: item.path).appendingPathComponent(item.name)
        var trashURL: NSURL?
        do {
            try FileManager.default.trashItem(at: source, resultingItemURL: &trashURL)
            originalURL = source
            trashedURL = trashURL as URL?
            canUndo = trashedURL != nil
            selected = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func undoDelete() {
        guard let trashedURL, let originalURL else { return }
        do {
            try FileManager.default.moveItem(at: trashedURL, to: originalURL)
            self.trashedURL = nil
            self.originalURL = nil
            canUndo = false
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
