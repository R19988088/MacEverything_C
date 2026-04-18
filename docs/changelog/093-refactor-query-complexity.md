# 093 - Refactor SearchEngine::query() complexity

## Summary

Refactored `SearchEngine::query()` from a monolithic ~640-line method with 7 levels of nesting into a thin dispatcher (~120 lines) plus 4 focused strategy methods and 3 static helpers. Pure structural refactoring with zero behavior change.

## Motivation

The original `query()` method was extremely difficult to understand and modify:
- ~640 lines in a single function
- 7 levels of nested control flow
- Duplicated code patterns (priority calculation x8, posting list intersection x4, path building x6)
- Mixed concerns: trigram lookup, name matching, path matching, slash-split handling, linear scan, sorting

## Changes

### Extracted static helpers

1. **`namePriority()`** — computes match priority (0=exact, 1=starts-with, 2=contains) from name data, eliminating 8 duplicated priority blocks.

2. **`intersectPostingLists()`** — intersects trigram posting lists for a keyword, eliminating 4 duplicated intersection blocks across name and path trigram lookups.

3. **`buildFullPathBuf()`** — assembles `path + '/' + name` into a reusable buffer with 2x growth strategy, eliminating 6 duplicated path-build blocks. Uses pre-lowered path data from `lowerPathPool_` (no runtime `simdToLowerAscii` needed).

### Extracted strategy methods

4. **`querySlashSplit()`** — handles queries containing `/` by splitting into path and name components, using both trigram indices for lookup.

5. **`queryPathTrigram()`** — handles path-only trigram matching for queries that didn't match via name trigrams, using `pathTrigramIndex_` for lookup.

6. **`queryLinearScanPath()`** — linear scan fallback for path matching when trigrams are unavailable or incomplete. Includes path deduplication optimization from `lowerPathPool_`.

7. **`queryLinearScan()`** — full linear scan for glob patterns and short keywords (< 3 chars). Uses GCD `dispatch_apply` for parallel execution with path dedup optimization.

### query() becomes a thin dispatcher

The main `query()` method now:
1. Validates input, acquires lock, bumps generation counter
2. Phase 1: trigram name matching (inline, ~20 lines)
3. Phase 2: delegates to one of the 4 strategy methods based on query characteristics
4. Sorts and truncates results

### Integration with lowerPathPool_

All strategy methods use `lowerPathPool_` for zero-cost lowercase path comparison, consistent with the optimization introduced in changelog 090. Path deduplication (pre-scan unique paths into a `pathMatchCache` vector for O(1) per-record lookup) is applied in both `queryLinearScanPath` and `queryLinearScan`.

## Files changed

- `MacEverything/Core/SearchEngine.h` — added `Match` struct, 4 strategy method declarations, 3 static helper declarations
- `MacEverything/Core/SearchEngine.cpp` — extracted helpers and strategy methods, rewrote `query()` as dispatcher

## Testing

- All 10,969 fast unit tests pass (0 failures)
- Search-specific tests pass: path search, trigram, query cancel, SIMD, lowerPathPool, ranking, path table
- HTTP API verified: trigram search, slash-split search, linear scan all return correct results
- Build and package succeed on master

## Risk

Zero — pure refactoring with no behavior change. All original logic preserved in the extracted methods.
