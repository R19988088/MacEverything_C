# 127 - Highlight Hints: AST-Based Search Result Highlighting

## Problem

The existing search result highlighting system had a fundamental architecture flaw: the Swift-side `QueryHighlightTokenizer.extractSearchKeywords()` re-parsed raw search text to extract highlight keywords. It only extracted `.word` and `.quoted` tokens, losing critical information:

- **Modifier filters** (case:Hello, regex:pattern, ww:test) — arguments classified as filterArg and discarded, causing no highlighting
- **Tilde queries** (~/Documents) — Swift side didn't expand `~` to `$HOME`
- **NOT operator** (!term) — negated words were still highlighted
- **path: filter** — arguments weren't highlighted

## Solution

Extract structured highlight hints directly from the C++ query AST, which already contains all the information needed for correct highlighting after `preprocessQuery()` → `QueryParser::parse()` → `transformSlashTerms()` → `transformGlobTerms()`.

### Data Flow

```
searchText → bridge.parseHighlightHints(query)
  → C++: preprocessQuery → parse → transforms → collectHighlightHints(ast)
  → ObjC: [MEHighlightHint] (text, field, matchMode, caseSensitive)
  → Swift: [HighlightHint]
  → ResultRow → highlightCrossMatches(path:name:hints:...)
```

## Implementation

### Step 1: C++ — HighlightHintExtractor.h (new file)

- `HintField` enum: NAME, PATH, ANY
- `HighlightHint` struct: text + field + mode + caseSensitive
- `collectHighlightHints()`: AST walker that skips NOT subtrees, collects TERM nodes, extracts path:/file: FILTER arguments
- `extractHighlightHints()`: Convenience function replicating the full query pipeline

### Step 2: Bridge — MEHighlightHint + parseHighlightHints:

- `MEHighlightHint` ObjC class with readonly properties (text, field, matchMode, caseSensitive as uint8_t)
- `parseHighlightHints:` method on MacSearchBridge calling C++ extractHighlightHints()

### Step 3: Swift — HighlightHint + ViewModel

- `HighlightHint` struct with `#if !TESTING` guard for standalone test compilation
- `highlightHints` computed property replaces old `highlightKeyword` in SearchViewModel

### Step 4: ResultRow + ContentView

- ResultRow: `keyword: String` → `hints: [HighlightHint]`
- ContentView: passes `viewModel.highlightHints` to ResultRow

### Step 5: TextHighlight — Hint-Based Highlighting

- `computeRangesForHint()`: mode-aware range computation (substring, glob, regex, wholeWord, wholeFilename)
- `computeHighlightRanges()`: aggregates ranges from multiple hints with merge/dedup
- `highlightMatches(in:hints:...)`: SwiftUI Text builder using hint-based ranges
- `highlightCrossMatches(path:name:hints:...)`: field-aware dispatching (NAME→name only, PATH→path only, ANY→both)
- `mapFullPathRanges()`: maps fullPath ranges back to path/name components

### Step 6: Cleanup

- Removed `highlightKeyword` from SearchViewModel (replaced by `highlightHints`)
- Kept `QueryHighlightTokenizer.extractSearchKeywords()` (still used by search input syntax coloring)
- `contentKeyword` unchanged (content search doesn't go through AST pipeline)

## Testing

### C++ Tests (Part 70 — tests/test_highlight_hints.h)

20+ test cases covering:
- Basic TERM, case:, regex:, ww:, wfn: modifiers
- NOT operator exclusion
- Multi-keyword, tilde expansion, glob patterns
- path:/file: filters, compound queries, slash queries
- Empty/whitespace queries, pure filters, quoted phrases, OR operator
- size: filter, mixed keyword+filters, nopath: exclusion

### Swift Tests (tests/test_highlight_ranges.swift)

22 pure function tests covering:
- Substring (case-insensitive and case-sensitive)
- Glob literal extraction and matching
- Regex pattern matching
- Whole word boundary matching
- Whole filename exact matching
- Multi-hint range merging
- Empty/no-match edge cases

### Integration Verification

- App built and launched successfully
- HTTP API verified with basic, case:, regex:, path:, ext:, glob, NOT queries
- All 11805 C++ tests passed

## Files Changed

| File | Change |
|------|--------|
| `MacEverything/Core/HighlightHintExtractor.h` | New — C++ hint extraction |
| `MacEverything/Bridge/MacSearchBridge.h` | Added MEHighlightHint + parseHighlightHints: |
| `MacEverything/Bridge/MacSearchBridge.mm` | Implemented bridge methods |
| `MacEverything/App/HighlightHint.swift` | New — Swift HighlightHint struct |
| `MacEverything/App/SearchViewModel.swift` | highlightKeyword → highlightHints |
| `MacEverything/App/ResultRow.swift` | keyword → hints parameter |
| `MacEverything/App/ContentView.swift` | Pass hints to ResultRow |
| `MacEverything/App/TextHighlight.swift` | Hint-based highlighting overloads |
| `tests/test_highlight_hints.h` | C++ unit tests (Part 70) |
| `tests/test_highlight_ranges.swift` | Swift pure function tests |
| `test_all.cpp` | Registered Part 70 |
| `Makefile` | Added -IMacEverything/Core flag |
