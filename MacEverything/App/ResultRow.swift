import SwiftUI
import AppKit

/// Shared icon cache to avoid repeated NSWorkspace.shared.icon(forFile:) calls.
/// - App bundles (type=5): cached by full path (each app has a unique icon)
/// - Regular files (type=1): cached by file extension (same ext → same icon)
/// - Directories/symlinks/other: cached by type (one icon per type)
final class FileIconCache {
    static let shared = FileIconCache()
    private let cache = NSCache<NSString, NSImage>()

    private init() {
        cache.countLimit = 500
    }

    func icon(for item: FileItem) -> NSImage {
        // Detect app bundles by name suffix (handles both type=5 and legacy type=2)
        let isApp = item.name.hasSuffix(".app")

        let key: String
        if isApp {
            // App bundles — unique icon per app
            key = "app:" + item.path + "/" + item.name
        } else if item.type == 1 {
            // Regular file — same icon per extension
            let ext = (item.name as NSString).pathExtension.lowercased()
            key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext)
        } else {
            // Dir, symlink, other — one icon per type
            key = "type:\(item.type)"
        }

        let nsKey = key as NSString
        if let cached = cache.object(forKey: nsKey) {
            return cached
        }

        let fullPath = item.path + "/" + item.name
        let nsImage = NSWorkspace.shared.icon(forFile: fullPath)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: nsKey)
        return nsImage
    }

    func icon(forPath path: String) -> NSImage {
        let ext = (path as NSString).pathExtension.lowercased()
        let key = "ext:" + (ext.isEmpty ? "__no_ext__" : ext) as NSString
        if let cached = cache.object(forKey: key) {
            return cached
        }
        let nsImage = NSWorkspace.shared.icon(forFile: path)
        nsImage.size = NSSize(width: 24, height: 24)
        cache.setObject(nsImage, forKey: key)
        return nsImage
    }
}

struct ResultRow: View, Equatable {
    @ObservedObject private var settings = AppSettings.shared
    let item: FileItem
    let hints: [HighlightHint]
    let iconScale: Double
    let isSelected: Bool
    let onSelect: () -> Void
    let onOpen: () -> Void
    let onReveal: () -> Void
    @State private var isHovered = false

    static func == (lhs: ResultRow, rhs: ResultRow) -> Bool {
        lhs.item == rhs.item && lhs.hints == rhs.hints && lhs.iconScale == rhs.iconScale && lhs.isSelected == rhs.isSelected
    }

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 0) {
                let highlighted = highlightCrossMatches(
                    path: item.path, name: item.name, hints: hints,
                    nameFont: .body, nameColor: .primary,
                    pathFont: .subheadline, pathColor: .secondary)

                HStack(spacing: 8) {
                    fileIcon(for: item)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: iconSize, height: iconSize)
                    highlighted.nameText.lineLimit(1)
                }
                .frame(minWidth: 220, maxWidth: .infinity, alignment: .leading)

                columnDivider
                highlighted.pathText
                    .font(.subheadline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(minWidth: 180, maxWidth: .infinity, alignment: .leading)

                columnDivider
                Text(item.type == 1 && item.size > 0 ? formatSize(item.size) : "—")
                    .font(.subheadline.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 92, alignment: .trailing)

                columnDivider
                Text(modifiedDate)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .frame(width: 178, alignment: .trailing)
            }
            .padding(.vertical, 7)
            .padding(.horizontal, 10)
            .background(
                RoundedRectangle(cornerRadius: 6)
                    .fill(isSelected ? Color.accentColor.opacity(0.24) : (isHovered ? Color.accentColor.opacity(0.12) : Color.clear))
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering in
            isHovered = hovering
        }
        .contextMenu {
            Button(AppText.value("action.open", language: settings.language)) { onOpen() }
            Button(AppText.value("action.reveal", language: settings.language)) { onReveal() }
            Divider()
            Button("Copy Path") { copyPath(item) }
        }
        .onDrag {
            let fullPath = item.path + "/" + item.name
            return NSItemProvider(object: NSURL(fileURLWithPath: fullPath))
        }
        .simultaneousGesture(TapGesture(count: 2).onEnded(onOpen))
        .accessibilityIdentifier("resultRow")
        .accessibilityValue(isSelected ? "selected" : "not-selected")
    }

    private var columnDivider: some View {
        Rectangle()
            .fill(Color.secondary.opacity(0.16))
            .frame(width: 1, height: 24)
            .padding(.horizontal, 10)
    }

    private var modifiedDate: String {
        guard item.modTime > 0 else { return "—" }
        return Self.dateFormatter.string(from: Date(timeIntervalSince1970: TimeInterval(item.modTime)))
    }

    private var iconSize: CGFloat { 24 * iconScale }

    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter
    }()

    private func fileIcon(for item: FileItem) -> Image {
        Image(nsImage: FileIconCache.shared.icon(for: item))
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var size = Double(bytes)
        var unitIndex = 0
        while size >= 1024 && unitIndex < units.count - 1 {
            size /= 1024
            unitIndex += 1
        }
        if unitIndex == 0 { return "\(bytes) B" }
        return String(format: "%.1f %@", size, units[unitIndex])
    }

    private func copyPath(_ item: FileItem) {
        let fullPath = item.path + "/" + item.name
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(fullPath, forType: .string)
    }
}
