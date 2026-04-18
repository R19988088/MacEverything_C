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

/// Cross-boundary highlight: matches keyword against `path + "/" + name` (the full path),
/// then maps matched ranges back to path and name individually.
/// This handles keywords like "test/goodprice" where "test" is at the end of `path`
/// and "goodprice" is the `name`.
///
/// Only activates cross-boundary logic when keyword contains "/"; otherwise falls back
/// to independent matching on each component.
func highlightCrossMatches(path: String, name: String, keyword: String,
                           nameFont: Font, nameColor: Color,
                           pathFont: Font, pathColor: Color,
                           highlightColor: Color = .accentColor) -> (nameText: Text, pathText: Text) {
    // Fast path: no keyword or no "/" in keyword — independent matching works fine
    if keyword.isEmpty || !keyword.contains("/") ||
       keyword.lowercased().contains("*") || keyword.lowercased().contains("?") {
        let nameText = highlightMatches(in: name, keyword: keyword, font: nameFont, color: nameColor, highlightColor: highlightColor)
        let pathText = highlightMatches(in: path, keyword: keyword, font: pathFont, color: pathColor, highlightColor: highlightColor)
        return (nameText, pathText)
    }

    let fullPath = path + "/" + name
    let lowerFull = fullPath.lowercased()
    let lowerKey = keyword.lowercased()

    // Find all matches in fullPath
    var fullRanges: [Range<String.Index>] = []
    var searchStart = lowerFull.startIndex
    while searchStart < lowerFull.endIndex,
          let range = lowerFull.range(of: lowerKey, range: searchStart..<lowerFull.endIndex) {
        fullRanges.append(range)
        searchStart = range.upperBound
    }

    if fullRanges.isEmpty {
        // No matches at all — return plain text
        return (Text(name).font(nameFont).foregroundColor(nameColor),
                Text(path).font(pathFont).foregroundColor(pathColor))
    }

    // The boundary between path and name in fullPath is at index path.count + 1 (the "/" separator)
    // fullPath = path + "/" + name
    //            0..pathLen-1  pathLen  pathLen+1..end
    // pathLen = path.count, separator at path.endIndex, name starts at separatorEnd
    let separatorIndex = fullPath.index(fullPath.startIndex, offsetBy: path.count)
    let nameStartInFull = fullPath.index(after: separatorIndex)

    // Map fullPath ranges to path ranges and name ranges
    var pathRanges: [Range<String.Index>] = []
    var nameRanges: [Range<String.Index>] = []

    for range in fullRanges {
        let matchStart = range.lowerBound
        let matchEnd = range.upperBound

        // Entirely within path (including the "/" separator shown as part of path display is tricky,
        // but the separator is not displayed in either path or name — path ends before "/",
        // name starts after "/". We clip to each component.)
        if matchEnd <= separatorIndex {
            // Entirely within path portion
            pathRanges.append(matchStart..<matchEnd)
        } else if matchStart >= nameStartInFull {
            // Entirely within name portion — remap to name's own indices
            let offset = fullPath.distance(from: nameStartInFull, to: matchStart)
            let length = fullPath.distance(from: matchStart, to: matchEnd)
            let nameStart = name.index(name.startIndex, offsetBy: offset)
            let nameEnd = name.index(nameStart, offsetBy: length)
            nameRanges.append(nameStart..<nameEnd)
        } else {
            // Cross-boundary: split into path part and name part
            // Path part: matchStart ..< separatorIndex (clip at path end)
            if matchStart < separatorIndex {
                pathRanges.append(matchStart..<separatorIndex)
            }
            // Name part: 0 ..< (matchEnd - nameStartInFull)
            if matchEnd > nameStartInFull {
                let nameLen = fullPath.distance(from: nameStartInFull, to: matchEnd)
                let nameEnd = name.index(name.startIndex, offsetBy: nameLen)
                nameRanges.append(name.startIndex..<nameEnd)
            }
        }
    }

    // Build highlighted Text for name
    let nameText = buildHighlightedText(text: name, ranges: nameRanges, font: nameFont, color: nameColor, highlightColor: highlightColor)
    // Build highlighted Text for path
    let pathText = buildHighlightedText(text: path, ranges: pathRanges, font: pathFont, color: pathColor, highlightColor: highlightColor)

    return (nameText, pathText)
}

/// Helper: build a SwiftUI Text with specified ranges highlighted.
private func buildHighlightedText(text: String, ranges: [Range<String.Index>],
                                  font: Font, color: Color,
                                  highlightColor: Color) -> Text {
    if ranges.isEmpty {
        return Text(text).font(font).foregroundColor(color)
    }

    var result = Text("")
    var currentIndex = text.startIndex

    for range in ranges {
        if currentIndex < range.lowerBound {
            result = result + Text(text[currentIndex..<range.lowerBound])
                .font(font).foregroundColor(color)
        }
        result = result + Text(text[range])
            .font(font).foregroundColor(highlightColor).bold()
        currentIndex = range.upperBound
    }

    if currentIndex < text.endIndex {
        result = result + Text(text[currentIndex..<text.endIndex])
            .font(font).foregroundColor(color)
    }

    return result
}
