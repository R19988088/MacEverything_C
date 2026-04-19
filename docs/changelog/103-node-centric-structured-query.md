# 103 - Node-Centric Structured Query System

## Summary

Implemented a node-centric query system that fundamentally changes how slash-containing queries work. Instead of treating `/abc/def` as a path-substring search (which returns noisy results including children and unrelated paths), the system now interprets slash queries as structured node searches: the last component identifies the target node by name, while preceding components act as path constraints.

## Query Modes

| Input | Mode | Semantics |
|---|---|---|
| `abc` | PLAIN | Unchanged behavior (name/path substring) |
| `/abc/def` | SEGMENTS | Name contains "def", parent path contains "abc" |
| `abc/def` | SEGMENTS | Same as above (leading slash optional) |
| `/abc/*/def` | SEGMENTS | Name contains "def", ancestor contains "abc" (non-adjacent) |
| `/abc/def/` | DIR_EXACT | Exact directory named "def" with parent containing "abc" |
| `/abc/def/*` | DIR_LIST | List direct children of directory "def" under "abc" |

## Implementation Details

### New Files
- **`MacEverything/Core/StructuredQueryParser.h`**: Pure inline parser that produces `ParsedQuery` struct with `QueryMode`, `namePattern`, and `pathSegments` (with adjacency tracking)
- **`tests/test_structured_query.h`**: 19 test cases (Part 58) covering parser unit tests and SearchEngine integration

### Modified Files
- **`SearchEngineQuery.cpp`**: Added `queryStructured()`, `queryDirList()`, and `pathSegmentsMatch()` implementations, plus query dispatch logic that routes structured queries before glob/trigram paths
- **`SearchEngine.h`**: Method declarations for the three new methods
- **`test_slash_query.h`**: Rewrote Part 48 tests for node-centric expectations
- **`test_path_search.h`**: Updated Part 3b/3b-2 slash query expectations
- **`test_path_trigram.h`**: Updated Part 47 slash query expectations
- **`test_memory_optimizations.h`**: Updated Part 21 `/home/user` expectation
- **`test_all.cpp`**: Part 58 registration

### Key Design Decisions
1. **Trigram-accelerated name search**: `queryStructured()` uses `nameTrigramIndex_` to find name candidates, then verifies with `simdContains()` and `pathSegmentsMatch()`
2. **Adjacency-aware path matching**: `pathSegmentsMatch()` walks path components right-to-left, respecting adjacency constraints (`*` breaks adjacency)
3. **DIR_LIST via pathLookup_**: `queryDirList()` finds directory records by exact name match, builds full path, looks up `pathLookup_` for pathPool index, then uses `pathIdxToRecords_` for O(1) children lookup
4. **Glob bypass**: `isGlobPattern()` is skipped for structured queries, preventing `/abc/*` from being treated as a glob pattern

### Bug Fixes During Implementation
- Fixed C99 designated initializer syntax (`{idx: ...}`) to valid C++20 aggregate initialization
- Fixed `records_[dirIdx].name` → `namePool_.data(dirIdx)` (InternalRecord has no `name` field)
- Fixed negative `phase1Ms` for structured queries by explicitly setting to 0.0

## Testing
- 11,398 fast tests pass (including 19 new Part 58 tests)
- HTTP API verified: SEGMENTS (`searchPath: structured`), DIR_LIST (`searchPath: dir-list`), and PLAIN (`searchPath: trigram`) all return correct results

## Performance
- SEGMENTS/DIR_EXACT: trigram name filter -> simdContains verify -> path segment check (much faster than full-path scan)
- DIR_LIST: directory name lookup + O(1) children via `pathIdxToRecords_` (vs. full linear scan before)
- PLAIN: completely unchanged
