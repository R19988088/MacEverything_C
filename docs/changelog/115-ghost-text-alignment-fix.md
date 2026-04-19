# 115 - Fix ghost text misalignment in search bar

## Problem

The search bar's ghost text (autocomplete suggestion from search history) was visually misaligned with the user's typed text. The ghost text appeared slightly to the left of where the actual characters rendered, creating a "double vision" / overlapping effect that made the UI look broken.

## Root Cause

The ghost text was rendered as a SwiftUI `Text` view in a `ZStack` alongside the `HighlightedSearchField` (which wraps an `NSTextView` inside `NSScrollView`). The two rendering systems have different text layout positions:

- `NSTextView` applies `textContainerInset` and `lineFragmentPadding` (default 5pt) that offset text from the view's leading edge
- SwiftUI `Text` starts at the exact leading edge of the ZStack, with no matching offsets

This inherent padding mismatch meant the ghost text and real text could never align perfectly.

## Solution

Moved ghost text rendering from the SwiftUI layer into the `NSTextView`'s `draw(_:)` method, using the identical `textContainerInset` + `lineFragmentPadding` offsets that the real text uses. This guarantees pixel-perfect alignment since both texts share the same coordinate system.

### Changes

1. **`HighlightedSearchField.swift`**
   - Added `ghostSuggestion: String?` parameter to `HighlightedSearchField`
   - Added `ghostSuggestion` property to `HighlightedNSTextView` with `needsDisplay` trigger
   - Added ghost text drawing in `draw(_:)` using the same inset calculation as the existing placeholder
   - Pass ghost suggestion from SwiftUI to NSTextView in `updateNSView()`

2. **`ContentView.swift`**
   - Removed the SwiftUI `Text` ghost overlay from the ZStack
   - Removed the now-unnecessary ZStack wrapper
   - Pass `ghostSuggestion` parameter to `HighlightedSearchField`

## Verification

- Build succeeded on master
- App launches and responds to search queries via HTTP API
- Ghost text now renders at the exact same position as typed text
