import SwiftUI
import AppKit

struct ContentResultRow: View, Equatable {
    @ObservedObject private var settings = AppSettings.shared
    let item: ContentFileItem
    let keyword: String
    let iconScale: Double
    let isSelected: Bool
    let onSelect: () -> Void
    let onOpen: () -> Void
    let onReveal: () -> Void
    @State private var isHovered = false

    static func == (lhs: ContentResultRow, rhs: ContentResultRow) -> Bool {
        lhs.item == rhs.item && lhs.keyword == rhs.keyword && lhs.iconScale == rhs.iconScale && lhs.isSelected == rhs.isSelected
    }

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 8) {
                fileIcon(for: item.filePath)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: iconSize, height: iconSize)

                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(item.fileName)
                            .font(.title3)
                            .fontWeight(.medium)
                            .foregroundColor(.primary)
                            .lineLimit(1)

                        Text(directoryPath(item.filePath))
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }

                    highlightMatches(in: item.snippet, keyword: keyword, font: .caption, color: .secondary)
                        .lineLimit(2)
                }

                Spacer()
            }
            .padding(.vertical, 5)
            .padding(.horizontal, 6)
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
            Button("Copy Path") { copyPath() }
        }
        .onDrag {
            return NSItemProvider(object: NSURL(fileURLWithPath: item.filePath))
        }
        .simultaneousGesture(TapGesture(count: 2).onEnded(onOpen))
        .accessibilityIdentifier("contentResultRow")
        .accessibilityValue(isSelected ? "selected" : "not-selected")
    }

    private func directoryPath(_ fullPath: String) -> String {
        if let range = fullPath.range(of: "/", options: .backwards) {
            return String(fullPath[fullPath.startIndex..<range.lowerBound])
        }
        return fullPath
    }

    private func fileIcon(for path: String) -> Image {
        Image(nsImage: FileIconCache.shared.icon(forPath: path))
    }

    private var iconSize: CGFloat { 24 * iconScale }

    private func copyPath() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(item.filePath, forType: .string)
    }
}
