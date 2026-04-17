# 068: Fix record deduplication — eliminate search result gaps

## Problem

Search result list displayed visible gaps (blank rows). Root cause: C++ SearchEngine allowed
multiple live records for the same file path. SwiftUI ForEach uses path-based IDs, so duplicate
IDs produced "ghost rows" that occupied vertical space but rendered nothing.

HTTP API verification confirmed the issue: searching "test" returned 50 results with only 30
unique paths (10 groups of duplicates).

## Root Cause

Three code paths could create orphaned duplicate records:

1. **`addRecord()`** — appended a new record and updated `pathIndex_` to point to it, but never
   tombstoned the previous record at the same path. The old record remained live (type != 0)
   but unreachable via `pathIndex_`.

2. **`loadRecords()`** — built `pathIndex_` with last-wins semantics, but set `liveCount_` to
   `records_.size()` without accounting for orphaned earlier records at duplicate paths.

3. **`compactRecords()`** — Phase 2 only skipped tombstones (type == 0), copying orphaned
   live-but-unreferenced records into the compacted output.

## Fix

### 1. `addRecord()` — dedup on insert (primary fix)

Before appending, check `pathIndex_` for an existing record at the same path. If found,
tombstone it (set type=0, clear fields, remove from trigram index, subtract from liveCount,
remove from recent cache). This mirrors the pattern already used in `replayWALEntries()`.

### 2. `loadRecords()` — dedup after pathIndex construction

After building `pathIndex_` (last-wins), collect all "winner" indices into a set. Scan all
records: any live record whose index is not in the winner set is an orphan — tombstone it.
Set `liveCount_` to the actual count of live records, not `records_.size()`.

### 3. `compactRecords()` — skip orphans in Phase 2

In the COW compaction loop, after checking `type != 0`, also verify the record's index matches
`snapPathIndex[fullPath]`. Records that are live but not referenced by the path index are
orphans from prior duplicate insertions — skip them.

### Test update

- `test_paged_persistence.h` P32-2: Updated expected liveRecordCount from 3073 to 3072, since
  `loadRecords` now correctly excludes tombstones from the live count.

## Tests

New test file `tests/test_record_dedup.h` (Part 45) with 6 test cases, 21 assertions:

1. addRecord dedup: same path twice, verify liveCount and latest-wins
2. loadRecords dedup: 4 records with 2 unique paths
3. compactRecords dedup: orphans removed, recordCount == liveRecordCount
4. addRecord vs replayWALEntries consistency
5. Query returns no duplicate paths (50 unique from 70 records)
6. Sequential addRecord same path x5

## Files Changed

| File | Change |
|------|--------|
| `MacEverything/Core/SearchEngine.cpp` | Dedup in addRecord, loadRecords, compactRecords |
| `tests/test_record_dedup.h` | New: 6 test cases for Part 45 |
| `tests/test_paged_persistence.h` | Updated P32-2 expected liveCount |
| `test_all.cpp` | Register Part 45 |

## Verification

- Part 45: 21/21 pass
- Full --fast regression: 10,803/10,803 pass
- Release build: succeeds
