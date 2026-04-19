# 122 - Unified Query Preprocessing Optimization

## Summary

Consolidate all `me::toLower()` calls into a single canonical computation in `preprocessQuery()`, eliminate redundant lowering throughout the query pipeline, fix a UB bug in the tokenizer, and remove dead code.

## Motivation

After extracting `preprocessQuery()` (#118), the downstream pipeline still had several inefficiencies:

1. **Triple lowering**: `transformSlashTerms()` → `parseQuery()` → `makeTerm()` each called `me::toLower()` on the same text
2. **Scattered manual loops**: `QueryTokenizer`, `QueryFilterParser`, and `QueryDateParser` used hand-rolled `std::tolower` character loops instead of the SIMD-accelerated `me::toLower()`
3. **UB bug**: `std::tolower(ch)` without `static_cast<unsigned char>` in `QueryTokenizer.h` is undefined behavior for non-ASCII input
4. **Wasted lowering**: `case:` and `regex:` modifiers computed `textLower` even though their matching paths don't use it
5. **Dead code**: `isGlobPattern()` was unused since AST-based glob handling was introduced

## Changes

### PreprocessedQuery struct (`SearchEngineQuery.cpp`)
- `preprocessQuery()` now returns `PreprocessedQuery{original, lower}` — a single `me::toLower()` at the entry point
- `query()` passes both fields downstream, eliminating re-computation

### parseQuery() overload (`StructuredQueryParser.h`)
- New two-argument overload `parseQuery(rawQuery, lowered)` accepts pre-computed lowercase
- Original single-argument overload preserved for backward compatibility

### makeTerm() overload (`QueryAST.h`)
- New three-argument overload `makeTerm(text, precomputedLower, mode)` skips `me::toLower()`
- Used in `transformSlashTerms()` where the name pattern is already lowered

### transformSlashTerms() (`ASTStructuredTransform.h`)
- Passes `node->textLower` to `parseQuery()` instead of re-lowering `node->text`
- Uses new `makeTerm` overload for the name term

### UB fix (`QueryTokenizer.h`)
- Replaced manual `std::tolower` character loop with `me::toLower(name)` for filter name lowering
- Fixes undefined behavior when query contains non-ASCII characters

### Consolidated lowering (`QueryFilterParser.h`, `QueryDateParser.h`)
- `path`/`nopath`/`parent` filter: uses `me::toLower(arg)` (was already correct, verified)
- `parseExt()`: bulk `me::toLower(arg)` then split by `;`
- `parseValueWithUnit()`: `me::toLower(s.substr(numEnd))` for unit suffix
- `parseDateExpr()`: `me::toLower(expr)` for keyword matching
- `case:` modifier: skips `textLower` computation (caseSensitive matching uses `text`)
- `regex:` modifier: skips `textLower` computation (regex uses `text` with `icase` flag)

### Dead code removal (`SearchEngineQuery.cpp`, `SearchEngine.h`)
- Removed `isGlobPattern()` definition and declaration — unused since `ASTGlobTransform.h` has its own inline check

### Tests (`tests/test_preprocess_unified.h`, Part 68)
- 16 test cases with 50 CHECK assertions covering all changes:
  - parseQuery two-arg and single-arg overloads
  - makeTerm with and without pre-computed lower
  - transformSlashTerms pipeline verification
  - case:/regex:/nocase: modifier behavior
  - ext/path/size filter lowering
  - tokenizer UB fix with filter names
  - SearchEngine integration (crash test)
  - Whitespace-only and tilde expansion
  - isGlobPattern removal (compile-time verified)

## Files Modified

| File | Change |
|------|--------|
| `MacEverything/Core/SearchEngineQuery.cpp` | `PreprocessedQuery` struct, updated `preprocessQuery()` and `query()` |
| `MacEverything/Core/SearchEngine.h` | Updated `queryAdvanced` decl, removed `isGlobPattern` decl |
| `MacEverything/Core/SearchEngineAdvancedQuery.cpp` | Updated `queryAdvanced` signature |
| `MacEverything/Core/StructuredQueryParser.h` | Two-arg `parseQuery` overload |
| `MacEverything/Core/QueryAST.h` | Three-arg `makeTerm` overload |
| `MacEverything/Core/ASTStructuredTransform.h` | Pass `textLower` to `parseQuery`, use new `makeTerm` |
| `MacEverything/Core/QueryTokenizer.h` | Fix UB → `me::toLower()` |
| `MacEverything/Core/QueryFilterParser.h` | Consolidate loops → `me::toLower()`, skip wasted lowering |
| `MacEverything/Core/QueryDateParser.h` | Consolidate loop → `me::toLower()` |
| `tests/test_preprocess_unified.h` | Part 68 tests (16 cases, 50 assertions) |
| `test_all.cpp` | Register Part 68 |

## Verification

- `./test_all --part 68`: 50/50 passed
- `./test_all --fast`: 11768 passed, 0 failed
- Release build + DMG packaging
- HTTP functional verification
