import SwiftUI

/// Shared highlight function used by ResultRow and ContentResultRow.
/// Finds all case-insensitive, non-overlapping occurrences of `keyword`
/// in `text` and renders matched segments in bold + `highlightColor`.
/// For glob-style keywords containing `*` or `?`, highlighting is skipped.
func highlightMatches(in text: String, keyword: String,
                      font: Font, color: Color,
                      highlightColor: Color = .accentColor) -> Text {
    guard !keyword.isEmpty else {
        return Text(text).font(font).foregroundColor(color)
    }

    let lowerText = text.lowercased()
    let lowerKey = keyword.lowercased()

    // Glob patterns — fall back to no highlighting
    if lowerKey.contains("*") || lowerKey.contains("?") {
        return Text(text).font(font).foregroundColor(color)
    }

    // Find all non-overlapping occurrences
    var ranges: [Range<String.Index>] = []
    var searchStart = lowerText.startIndex
    while searchStart < lowerText.endIndex,
          let range = lowerText.range(of: lowerKey, range: searchStart..<lowerText.endIndex) {
        ranges.append(range)
        searchStart = range.upperBound
    }

    if ranges.isEmpty {
        return Text(text).font(font).foregroundColor(color)
    }

    // Build attributed Text by segments
    var result = Text("")
    var currentIndex = text.startIndex

    for range in ranges {
        // Non-matched segment before this match
        if currentIndex < range.lowerBound {
            result = result + Text(text[currentIndex..<range.lowerBound])
                .font(font).foregroundColor(color)
        }
        // Matched segment (use original case from text)
        result = result + Text(text[range])
            .font(font).foregroundColor(highlightColor).bold()
        currentIndex = range.upperBound
    }

    // Remaining text after last match
    if currentIndex < text.endIndex {
        result = result + Text(text[currentIndex..<text.endIndex])
            .font(font).foregroundColor(color)
    }

    return result
}
