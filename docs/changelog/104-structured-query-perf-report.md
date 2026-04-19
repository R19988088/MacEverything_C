# 104 - Structured Query Performance Report

## Summary

Performance benchmark of the node-centric structured query system (Feature 103) against a real 4.45M-record index. Tests cover all four query modes: PLAIN, SEGMENTS, DIR_EXACT, DIR_LIST.

## Test Environment

- **Records**: 4,457,162 live / 5,027,055 total
- **Machine**: macOS (Darwin 24.3.0)
- **Build**: Release (-O2), clang++/c++20
- **Date**: 2026-04-19

## Results Summary

### Fast Queries (< 5ms) - Working Well

| Query | Mode | Avg(ms) | Results | Notes |
|---|---|---|---|---|
| `SearchEngine` | PLAIN/trigram | 0.55 | 100 | Trigram high selectivity |
| `StructuredQueryParser` | PLAIN/trigram | 0.17 | 1 | Rare keyword, near-instant |
| `qzxwvuts_nonexist` | PLAIN/trigram | 0.03 | 0 | Trigram early exit |
| `/brew` | SEGMENTS | 0.24 | 100 | 4-char name, trigram effective |
| `local/brew` | SEGMENTS | 0.72 | 100 | Name trigram + path constraint |
| `/usr/bin/zsh` | SEGMENTS | 0.95 | 0 | 3-char name, trigram works (selective) |
| `Core/SearchEngine` | SEGMENTS | 3.20 | 100 | Long name, trigram + path |
| `/etc/*` | DIR_LIST | 0.22 | 100 | O(1) children lookup |
| `/usr/local/*` | DIR_LIST | 1.69 | 31 | O(1) lookup, few children |
| `/etc/` | DIR_EXACT | 0.21 | 100 | Exact match, fast |
| `/usr/local/` | DIR_EXACT | 1.79 | 8 | Exact match |

### Slow Queries (> 100ms) - Performance Issues Identified

| Query | Mode | Avg(ms) | Results | Root Cause |
|---|---|---|---|---|
| `test` | PLAIN/linear | 119.77 | 100 | Trigram candidates too many, linear fallback |
| `ls` | PLAIN/linear | 152.22 | 100 | 2-char, linear scan required |
| `bin/ls` | SEGMENTS | 457.22 | 100 | Name "ls" 2-char, full linear scan + pathSegmentsMatch per record |
| `local/python` | SEGMENTS | 413.13 | 100 | Name "python" trigram too many candidates, linear fallback |
| `/usr/local/bin` | SEGMENTS | 623.55 | 100 | Name "bin" 3-char but too common, linear + path check |
| `usr/bin` | SEGMENTS | 658.91 | 100 | Same: "bin" too common for trigram |
| `/usr/bin/*` | DIR_LIST | 66.26 | 100 | "bin" is a common dir name, many matching dirs to check |

## Root Cause Analysis

### Primary Issue: Linear Scan Fallback in Structured Queries

The structured query path (`queryStructured()`) falls back to linear scan when:
1. `namePattern` is < 3 characters (cannot use trigram index)
2. Trigram candidates exceed `totalSize / 67` threshold (~67K for 4.5M records)

**Why structured linear scan is slower than PLAIN linear scan:**

In PLAIN linear scan, the per-record cost is primarily `simdContains(name)` (~few ns).

In structured linear scan, each record additionally requires:
- `lowerPathPool_.str(pathIndices_[i])` — constructs a `std::string` from the path pool (memory allocation!)
- `pathSegmentsMatch()` — walks path components right-to-left with string operations

This makes structured linear scan **3-4x slower** than PLAIN linear scan for the same dataset:
- PLAIN `ls` (2-char): ~150ms
- SEGMENTS `bin/ls` (name "ls"): ~457ms

### Secondary Issue: Negative phase1Ms in PLAIN trigram-split path

When PLAIN queries fall to `trigram-split` (trigram intersection runs but candidates exceed threshold), `phase1Ms` reports negative values (e.g., -2.84ms). The fix from Feature 103 only covers structured queries, not this PLAIN-mode path.

## Performance Characteristics by Mode

### PLAIN Mode
- **Trigram (selective keywords)**: < 1ms — excellent
- **Linear fallback (common keywords/short queries)**: 100-250ms — acceptable for interactive use
- **No match**: < 0.1ms — trigram early exit

### SEGMENTS Mode
- **With trigram**: < 5ms — excellent
- **Linear fallback**: 400-700ms — **too slow for interactive use**
- **No match**: < 0.1ms

### DIR_LIST Mode
- **Small directories**: < 2ms — excellent (O(1) pathIdxToRecords_ lookup)
- **Common dir names (e.g., "bin")**: ~66ms — moderate, multiple dirs to enumerate
- **No match**: < 0.1ms

### DIR_EXACT Mode
- **All cases tested**: < 2ms — excellent (exact name match very selective)

## Optimization Opportunities

1. **Path trigram index for structured queries**: When name trigram fails, use `pathTrigramIndex_` to narrow candidates before linear scan. For `usr/bin`, instead of scanning all 4.5M records, first intersect path trigrams for path segments (e.g., "usr") to get a much smaller candidate set.

2. **Avoid string allocation in pathSegmentsMatch**: Currently `lowerPathPool_.str()` allocates a new `std::string`. Could use a `std::string_view`-based approach or match directly against the pool buffer.

3. **Early termination with maxResults**: When enough results are found (e.g., 100), stop scanning. Currently the linear scan continues through all records even after finding enough matches.

4. **Combined name+path trigram intersection**: For queries like `usr/bin`, intersect path trigrams for "usr" with name trigrams for "bin" to get a very small candidate set.

## Conclusion

The node-centric query system performs excellently when trigram acceleration is effective (< 5ms for SEGMENTS, < 2ms for DIR_LIST/DIR_EXACT). The main performance gap is in the linear scan fallback for short or common name patterns, where structured queries are 3-4x slower than PLAIN mode due to per-record path string construction and segment matching. This is a known limitation and potential area for future optimization.
