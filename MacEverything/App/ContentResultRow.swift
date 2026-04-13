import SwiftUI
import AppKit

struct ContentResultRow: View {
    let item: ContentFileItem
    let keyword: String
    @State private var isHovered = false

    var body: some View {
        HStack(spacing: 8) {
            fileIcon(for: item.filePath)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 24, height: 24)

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

                highlightedSnippet(item.snippet, keyword: keyword)
                    .lineLimit(2)
            }

            Spacer()
        }
        .padding(.vertical, 5)
        .padding(.horizontal, 6)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isHovered ? Color.accentColor.opacity(0.12) : Color.clear)
        )
        .contentShape(Rectangle())
        .onHover { hovering in
            isHovered = hovering
        }
        .contextMenu {
            Button("Open") { openFile() }
            Button("Reveal in Finder") { revealInFinder() }
            Divider()
            Button("Copy Path") { copyPath() }
        }
        .onTapGesture(count: 2) { openFile() }
    }

    private func directoryPath(_ fullPath: String) -> String {
        if let range = fullPath.range(of: "/", options: .backwards) {
            return String(fullPath[fullPath.startIndex..<range.lowerBound])
        }
        return fullPath
    }

    private func highlightedSnippet(_ snippet: String, keyword: String) -> Text {
        guard !keyword.isEmpty else {
            return Text(snippet).font(.caption).foregroundColor(.secondary)
        }

        let lowerSnippet = snippet.lowercased()
        let lowerKey = keyword.lowercased()

        var ranges: [Range<String.Index>] = []
        var searchStart = lowerSnippet.startIndex
        while searchStart < lowerSnippet.endIndex,
              let range = lowerSnippet.range(of: lowerKey, range: searchStart..<lowerSnippet.endIndex) {
            ranges.append(range)
            searchStart = range.upperBound
        }

        if ranges.isEmpty {
            return Text(snippet).font(.caption).foregroundColor(.secondary)
        }

        var result = Text("")
        var currentIndex = snippet.startIndex

        for range in ranges {
            if currentIndex < range.lowerBound {
                result = result + Text(snippet[currentIndex..<range.lowerBound])
                    .font(.caption).foregroundColor(.secondary)
            }
            result = result + Text(snippet[range])
                .font(.caption).foregroundColor(.accentColor).bold()
            currentIndex = range.upperBound
        }

        if currentIndex < snippet.endIndex {
            result = result + Text(snippet[currentIndex..<snippet.endIndex])
                .font(.caption).foregroundColor(.secondary)
        }

        return result
    }

    private func fileIcon(for path: String) -> Image {
        Image(nsImage: FileIconCache.shared.icon(forPath: path))
    }

    private func openFile() {
        NSWorkspace.shared.open(URL(fileURLWithPath: item.filePath))
    }

    private func revealInFinder() {
        NSWorkspace.shared.selectFile(item.filePath, inFileViewerRootedAtPath: "")
    }

    private func copyPath() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(item.filePath, forType: .string)
    }
}
