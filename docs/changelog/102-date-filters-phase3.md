# 102 — Phase 3: Date Filters (dm:, dc:, da:)

## Summary

Implemented Phase 3 of the Everything search syntax plan: date-based filtering using `dm:` (date modified), `dc:` (date created), and `da:` (date accessed) filter prefixes. Also supports long aliases `datemodified:`, `datecreated:`, `dateaccessed:`.

## New Files

- **`MacEverything/Core/QueryDateParser.h`** (~305 lines, header-only)
  - Parses date filter arguments into `CompareOp` + epoch timestamps on `QueryNode`
  - Supported date expressions:
    - Keywords: `today`, `yesterday`, `thisweek`, `lastweek`, `thismonth`, `lastmonth`, `thisyear`, `lastyear`
    - Relative: `last7days`, `last30days`, `last3months`, `last6months`, `last1year`
    - ISO dates: `2024`, `2024-06`, `2024-06-15`
    - Comparisons: `>2024-01-01`, `>=today`, `<2024-06`, `<=yesterday`
    - Ranges: `2024-01..2024-06`

- **`tests/test_query_date_filters.h`** — Part 57, 71 tests
  - Section A (A1–A19): QueryDateParser unit tests for all date formats
  - Section B (B1–B15): Integrated SearchEngine tests with backdated files

## Modified Files

- **`MacEverything/Core/QueryFilterParser.h`** — Added routing for dm/dc/da filters to `QueryDateParser::parse()`
- **`MacEverything/Core/SearchEngineAdvancedQuery.cpp`** — Added `evalFilter()` branches for dm/dc/da using `compareNumeric()` against `rec.modTime`
- **`test_all.cpp`** — Added Part 57 include, dispatch, and `QueryFilterParser.h` include
- **`MacEverything.xcodeproj/project.pbxproj`** — Added `QueryDateParser.h` to Xcode project

## Design Decisions

1. **Header-only QueryDateParser**: All date parsing logic in a single header for simplicity; no .cpp needed since it's pure computation with no external dependencies beyond `<ctime>`.
2. **Epoch-based comparison**: All dates are converted to `time_t` (Unix epoch) and stored as `numVal1`/`numVal2` on `QueryNode`. The existing `compareNumeric()` in `evalFilter()` handles all comparison operators.
3. **dc: uses modTime fallback**: macOS supports `birthtime` (creation time), but `FileRecord` currently only stores `modTime`. `dc:` falls back to `modTime` until `birthtime` is added to scanning.
4. **Week starts on Sunday**: `thisweek`/`lastweek` use Sunday as week start, consistent with `tm_wday` convention.

## Test Results

- Part 57: **71 passed, 0 failed**
- Full `--fast` suite: **11,348 passed, 0 failed**
