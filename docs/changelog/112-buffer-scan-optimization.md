# 112 - Buffer-Scan & Path-Trigram Optimization for Structured Query Fallback

## Problem

Structured queries like `bin/ls` took ~440ms because:
- `namePattern` "ls" has only 2 characters, so no trigram index can be used
- `pathSegment` "bin" has too many candidates in `nameTrigramIndex_` (>66K), exceeding the trigram threshold
- Falls back to linear scan of all 5M records with 4-way scattered memory access (records_, namePool_.entries_, namePool_.buffer_, pathIndices_)

## Root Cause Analysis

The linear scan bottleneck is **cache inefficiency**: 4 independent memory streams across 5M iterations generate ~6900K cache line accesses. The CPU prefetcher cannot keep up with 4 unrelated streams.

## Solution: Three-Layer Optimization

### Layer 1: Buffer-Scan for Bare Name Queries (no path segments)
When no path segments are present, SIMD-scan the contiguous `namePool_.buffer_` (~75MB) using `simdFindAll()`, then resolve hit byte offsets to record indices via forward cursor walk.

- `namePool_.buffer_` is a single contiguous `vector<char>` — perfect for SIMD streaming
- Hit offsets are ascending, entries are sorted by offset — cursor walk is O(hits + entries) with no binary search
- Cross-boundary false positives are rejected by validating `hitOffset + patternLen <= entry.offset + entry.length`

### Layer 2: Path-First Scan (with path segments, no trigram)
When path segments exist but no segment has >= 3 characters for trigrams, iterate all ~100K paths in `lowerPathPool_`, filter via `pathSegmentsMatch()`, then iterate only records under matching paths via `pathIdxToRecords_`.

### Layer 3: Path-Trigram Accelerated Scan (with path segments >= 3 chars)
Use the existing `pathTrigramIndex_` to narrow candidate paths from ~100K to ~1K before applying `pathSegmentsMatch()`. For "bin/ls":
- `intersectPostingLists(pathTrigramIndex_, "bin")` returns ~1K candidate path indices
- Only those candidates are checked with `pathSegmentsMatch()` and expanded via `pathIdxToRecords_`

## Performance Results

| Query | Before | After | Speedup |
|---|---|---|---|
| `bin/ls` | 440ms | **7-30ms** | 15-60x |
| `usr/lib` | ~400ms | **1.8ms** | ~220x |
| `etc/conf` | ~400ms | **18ms** | ~22x |
| `local/python` | 21ms | **3-4ms** | No regression (path anchor strategy) |

## Files Changed

- `MacEverything/Core/SearchEngineStructuredQuery.cpp` — Replaced `dispatch_apply` linear scan with:
  1. Path-trigram narrowing via `pathTrigramIndex_`
  2. Path-first iteration via `pathIdxToRecords_`
  3. Buffer-scan + cursor walk for bare name queries
- `tests/test_structured_query.h` — Added tests 29-32: buffer-scan correctness, cross-boundary rejection, tombstone exclusion, DIR_EXACT

## Testing

- All 11551 unit tests pass
- Integration tested via HTTP API (`curl http://localhost:19860/api/search?q=bin/ls`)
- No regressions observed in other query paths (trigram, path anchor, tree walk)
