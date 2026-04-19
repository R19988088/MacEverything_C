# 107 — Structured Query Linear Scan Optimizations

## Background

Feature 104 benchmark revealed that SEGMENTS queries with short/common name patterns fall back to a linear scan of all ~4.5M records, taking 400-660ms. The linear scan had several inefficiencies:

1. Single-threaded — no parallelism on multi-core machines
2. Per-record path segment matching — `pathSegmentsMatch()` called for every record, even though most records share the same ~100K unique directory paths
3. Redundant lowering — `pathSegmentsMatch()` manually lowered its input, but input from `lowerPathPool_` was already lowercase
4. Heap allocation — `lowerPathPool_.str()` returned `std::string` (heap alloc) per record instead of zero-copy `string_view`

## Changes

### Optimization 1: `dispatch_apply` (GCD) Parallelism
- Replaced single-threaded loop with `dispatch_apply` using `hardware_concurrency()` threads (capped at 32)
- Each thread processes a chunk of records into a local `vector<Match>`, merged after completion
- Matches the existing pattern in `queryLinearScanPath()` and `queryLinearScan()`

### Optimization 2: `pathMatchCache` Pre-computation
- Pre-compute path segment matching over ~100K unique paths once (O(100K))
- Store results in `vector<bool> pathMatchCache` indexed by pathIdx
- Each record lookup becomes O(1) via `pathMatchCache[pathIndices_[i]]` instead of calling `pathSegmentsMatch()` per record

### Optimization 3: Remove Redundant Lowering
- Changed `pathSegmentsMatch()` parameter from `const std::string&` to `std::string_view`
- Removed the manual lowercase conversion loop inside the function
- Input from `lowerPathPool_` is already lowercase, so the lowering was pure waste

### Optimization 4: `string_view` Instead of `std::string`
- `queryStructuredNameAnchor()` and `queryStructuredPathAnchor()` now pass `lowerPathPool_.view()` directly to `pathSegmentsMatch()` instead of constructing temporary `std::string`
- Eliminates heap allocation per candidate record

## Files Modified

- `MacEverything/Core/SearchEngine.h` — Updated `pathSegmentsMatch` declaration to accept `string_view`
- `MacEverything/Core/SearchEngineStructuredQuery.cpp` — All 4 optimizations applied to `queryStructured()`, `queryStructuredNameAnchor()`, `queryStructuredPathAnchor()`, and `pathSegmentsMatch()`
- `tests/test_structured_query.h` — Added tests 26-28 for parallel scan, pathMatchCache, and lowercase path matching

## Tests

- Test 26: Parallel linear scan consistency — verifies `dispatch_apply` produces correct results
- Test 27: pathMatchCache deduplication — 50 files sharing same dir, correct filtering
- Test 28: Already-lowercase path matching — verifies no redundant lowering needed
- All 11419 tests pass

## Expected Performance Impact

These optimizations target the linear scan fallback path, which activates when no segment has good trigram selectivity. Combined effect:
- Multi-threading: ~Nx speedup on N-core machines
- pathMatchCache: reduces per-record work from O(path_components) to O(1)
- Eliminated lowering + string_view: removes heap allocation and redundant computation per record
