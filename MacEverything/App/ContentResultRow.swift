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
    @State private var lastTapUptime: TimeInterval = 0

    static func == (lhs: ContentResultRow, rhs: ContentResultRow) -> Bool {
        lhs.item == rhs.item && lhs.keyword == rhs.keyword && lhs.iconScale == rhs.iconScale && lhs.isSelected == rhs.isSelected && lhs.settings.themeColorRawValue == rhs.settings.themeColorRawValue
    }

    var body: some View {
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

                highlightMatches(in: item.snippet, keyword: keyword, font: .caption, color: .secondary,
                                 highlightColor: Color(hex: settings.themeColorHex))
                    .lineLimit(2)
            }

            Spacer()
        }
        .padding(.vertical, 5)
        .padding(.horizontal, 6)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color(hex: settings.themeColorHex).opacity(0.24) : (isHovered ? Color(hex: settings.themeColorHex).opacity(0.12) : Color.clear))
        )
        .contentShape(Rectangle())
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
        .onTapGesture(perform: handleTap)
        .accessibilityIdentifier("contentResultRow")
        .accessibilityAddTraits(.isButton)
        .accessibilityValue(isSelected ? "selected" : "not-selected")
    }

    private func directoryPath(_ fullPath: String) -> String {
        if let range = fullPath.range(of: "/", options: .backwards) {
            return String(fullPath[fullPath.startIndex..<range.lowerBound])
        }
        return fullPath
    }

    private func handleTap() {
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastTapUptime < 0.35 {
            onOpen()
            lastTapUptime = 0
        } else {
            onSelect()
            lastTapUptime = now
        }
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
