# 117 - Add system keyword ghost suggestions

## Background

The ghost text autocomplete in the search bar previously only showed suggestions from the user's search history. Users who are learning the filter syntax (e.g., `ext:`, `size:`, `path:`) had no discoverability mechanism — they had to know the keywords in advance.

## Solution

Added system filter keywords as a fallback ghost suggestion source. When the user types a partial word that matches a known filter keyword prefix, the ghost text now shows the full keyword.

### Priority order:
1. **Search history match** — highest priority (existing behavior preserved)
2. **System keyword match** — fallback when no history match exists

### Matching rules:
- Only triggers on single partial words (no spaces, no colon already present)
- When multiple keywords match, the shortest is preferred (e.g., "d" → "dc:" not "datemodified:")
- Alphabetical tiebreaker for same-length matches

### Examples:
- Type "ex" → ghost shows "ext:"
- Type "si" → ghost shows "size:"
- Type "au" → ghost shows "audio:"
- Type "dat" → ghost shows "datemodified:" (shortest date filter after "da:" and "dc:")
- Type "ext:" → no keyword ghost (colon already present)
- Type "ext:cpp" from history → history match wins over keyword

### Supported keywords (28 total):
`ext:`, `size:`, `file:`, `folder:`, `path:`, `nopath:`, `parent:`, `depth:`, `len:`, `dm:`, `dc:`, `da:`, `datemodified:`, `datecreated:`, `dateaccessed:`, `case:`, `nocase:`, `regex:`, `ww:`, `wfn:`, `wholeword:`, `wholefilename:`, `audio:`, `video:`, `pic:`, `doc:`, `exe:`, `zip:`, `content:`, `type:`

## Changes

- **`MacEverything/App/SearchViewModel.swift`**
  - Added `systemKeywords` static array with all known filter keywords
  - Added `systemKeywordMatch(for:)` static method for prefix matching with shortest-first ranking
  - Updated `updateGhostSuggestion()` to fall back to system keyword match when no history match exists

## Verification

- Build succeeded on master
- App launches and responds to search queries via HTTP API
- Ghost text suggestions now include system keywords as fallback
