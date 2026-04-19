# 119 — Add Whitespace Trim to `preprocessQuery()`

## Summary

Added leading/trailing whitespace stripping as the first step in
`preprocessQuery()`, ensuring that queries like `"  hello  "` behave
identically to `"hello"` regardless of input source.

## Motivation

User input may contain accidental whitespace — from the search bar,
HTTP API, or clipboard paste. Without trimming, this whitespace could
affect tilde expansion (a leading space prevents `~` detection), glob
pattern matching, and slash-based path detection. Centralising the
trim in `preprocessQuery()` provides consistent behaviour across all
query paths.

## Changes

### `MacEverything/Core/SearchEngineQuery.cpp`

- **Added** step 0 in `preprocessQuery()`: strip leading/trailing
  whitespace (`" \t\r\n"`) using `find_first_not_of` /
  `find_last_not_of`. All-whitespace input returns an empty string.
- **Added** early-return guard in `query()`: after `preprocessQuery()`
  returns, check for empty result and return `{}` immediately. This
  handles the case where the raw input is non-empty but
  all-whitespace.

### `tests/test_whitespace_trim.h` (Part 66)

New test file with 7 test cases:
1. `"  hello  "` matches same count as `"hello"`
2. `"\t hello \n"` matches same count as `"hello"`
3. `"   "` (all-whitespace) returns 0 results
4. `" "` (single space) returns 0 results
5. Interior space preserved: `"  hello world  "` same as `"hello world"`
6. Trim + tilde combined: `"  ~/Downloads/*.txt  "` same as `"~/Downloads/*.txt"`
7. Trim + tilde result correctness: matches `f1.txt`

### `test_all.cpp`

- Added `#include "tests/test_whitespace_trim.h"`
- Registered Part 66 in dispatch table and `--fast` suite
- Updated help text

## Verification

- **Unit tests**: Parts 65 + 66 (14 tests) — all pass.
- **Fast suite**: 11,685 tests — all pass, 0 failures.
- **Build**: `xcodebuild` Release build succeeded.
- **HTTP**: Verified via `curl` with whitespace-padded queries.

## Risk

Minimal — pure input normalisation. All existing tests continue to
pass. The only behavioural change is that all-whitespace queries now
return empty results immediately instead of flowing through the full
query pipeline.
