# SearchEngine::query() Refactor — Branch Complexity Reduction

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce the ~640-line `query()` method from 7 nesting levels to a flat ~80-line dispatcher by extracting reusable helpers and strategy functions, with zero behavior change.

**Architecture:** Extract duplicated logic into private helper methods (`namePriority`, `verifyAndScore`, `intersectPostingLists`), then lift each search strategy (trigram name, slash-split, path trigram, linear scan fallback, full linear scan) into its own private method. The main `query()` becomes a dispatcher that selects a strategy and calls it.

**Tech Stack:** C++20, ARM NEON SIMD (`SIMDSearch.h`), GCD `dispatch_apply`, `StringPool`, trigram index.

---

## File Structure

All changes are within existing files — no new files created.

- **Modify:** `MacEverything/Core/SearchEngine.h` — add private helper method declarations
- **Modify:** `MacEverything/Core/SearchEngine.cpp` — extract helpers, refactor `query()`
- **Test:** `tests/test_slash_query.h` (10 tests), `tests/test_path_search.h` (8 tests), `tests/test_query_perf.h` (7 scenarios) — all must pass unchanged
- **Test:** `tests/test_trigram_index.h`, `tests/test_ranking.h` — additional regression coverage

---

### Task 1: Baseline — Compile and Run Existing Tests

Establish a green baseline before any refactoring.

**Files:**
- Read: `test_all.cpp`, `MacEverything/Core/SearchEngine.cpp`

- [ ] **Step 1: Compile the test binary**

```bash
cd /Users/wujian/data/project/mac_everything
clang++ -std=c++20 -O2 -framework CoreServices -framework CoreFoundation MacEverything/Core/*.cpp test_all.cpp -o test_all
```

- [ ] **Step 2: Run the relevant test parts**

```bash
./test_all --part 3b --part 44 --part 48
```

Expected: All tests PASS. Record the exact pass/fail counts for comparison after refactoring.

- [ ] **Step 3: Commit baseline (no code changes)**

No commit needed — this is a verification step only.

---

### Task 2: Extract `namePriority()` Helper

This helper eliminates 7 duplicated priority-assignment blocks. Each block does:
1. Check if name exactly equals keyword → priority 0
2. Check if name starts with keyword → priority 1
3. Otherwise → priority 2

**Files:**
- Modify: `MacEverything/Core/SearchEngine.h:238+` (add private declaration)
- Modify: `MacEverything/Core/SearchEngine.cpp` (add implementation, replace 7 call sites)

- [ ] **Step 1: Add declaration to SearchEngine.h**

Add in the `private:` section, after `removeFromRecentCache`:

```cpp
    /// Compute match priority: 0=exact, 1=starts-with, 2=contains.
    /// Assumes nameData is already lowercase and matches lowerKey via simdContains.
    static uint8_t namePriority(const char* nameData, uint16_t nameLen,
                                const char* keyData, size_t keyLen);
```

- [ ] **Step 2: Add implementation to SearchEngine.cpp**

Add after the `makeFullPath` function (around line 52):

```cpp
uint8_t SearchEngine::namePriority(const char* nameData, uint16_t nameLen,
                                   const char* keyData, size_t keyLen) {
    if (nameLen == keyLen && memcmp(nameData, keyData, nameLen) == 0)
        return 0; // exact match
    if (nameLen >= keyLen && memcmp(nameData, keyData, keyLen) == 0)
        return 1; // starts with
    return 2; // contains
}
```

- [ ] **Step 3: Replace all 7 inline priority blocks with `namePriority()` calls**

**Call site 1 — Phase 1 trigram candidate verification (lines 464-472):**

Replace:
```cpp
                uint8_t priority;
                if (nameLen == lowerKey.size() && memcmp(nameData, lowerKey.data(), nameLen) == 0) {
                    priority = 0;
                } else if (nameLen >= lowerKey.size() &&
                           memcmp(nameData, lowerKey.data(), lowerKey.size()) == 0) {
                    priority = 1;
                } else {
                    priority = 2;
                }
```
With:
```cpp
                uint8_t priority = namePriority(nameData, nameLen, lowerKey.data(), lowerKey.size());
```

**Call site 2 — Slash-split both-indexes path (lines 604-606):**

Replace the ternary chain:
```cpp
                            uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                                ? ((nl == lowerKey.size() && memcmp(nd, lowerKey.data(), nl) == 0) ? 0 : (nl >= lowerKey.size() && memcmp(nd, lowerKey.data(), lowerKey.size()) == 0 ? 1 : 2))
                                : 3;
```
With:
```cpp
                            uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                                ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
```

**Call site 3 — Slash-split path-only supplement loop (lines 644-646):**

Same ternary replacement as call site 2.

**Call site 4 — Slash-split path-only-index branch (lines 677-679):**

Same ternary replacement as call site 2.

**Call site 5 — Slash-split name-only-index branch (lines 702-704):**

Same ternary replacement as call site 2.

**Call site 6 — Path trigram Phase 2 (lines 775-779):**

Replace:
```cpp
                        if (me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())) {
                            if (nl == lowerKey.size() && memcmp(nd, lowerKey.data(), nl) == 0) priority = 0;
                            else if (nl >= lowerKey.size() &&
                                     memcmp(nd, lowerKey.data(), lowerKey.size()) == 0) priority = 1;
                            else priority = 2;
                        } else {
                            priority = 3;
                        }
```
With:
```cpp
                        priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                            ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
```

**Call site 7 — Linear scan full (lines 928-935):**

Replace:
```cpp
                    if (nameMatch) {
                        if (nl == lowerKey.size() && memcmp(nd, lowerKey.data(), nl) == 0) priority = 0;
                        else if (nl >= lowerKey.size() &&
                                 memcmp(nd, lowerKey.data(), lowerKey.size()) == 0) priority = 1;
                        else priority = 2;
                    } else {
                        priority = 3;
                    }
```
With:
```cpp
                    priority = nameMatch ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
```

Note: Call sites in the trigram Phase 2 linear scan (lines 832-836) also need replacement — same pattern.

- [ ] **Step 4: Compile and run tests**

```bash
clang++ -std=c++20 -O2 -framework CoreServices -framework CoreFoundation MacEverything/Core/*.cpp test_all.cpp -o test_all
./test_all --part 3b --part 44 --part 48
```

Expected: Same pass/fail counts as baseline.

- [ ] **Step 5: Commit**

```bash
git add MacEverything/Core/SearchEngine.h MacEverything/Core/SearchEngine.cpp
git commit -m "refactor: extract namePriority() helper, eliminate 7+ duplicated priority blocks"
```

---

### Task 3: Extract `intersectPostingLists()` Helper

This eliminates 4 duplicated trigram posting-list intersection blocks. Each block does:
1. Collect posting lists from a trigram index
2. Sort by size (smallest first)
3. Intersect via `std::set_intersection`

**Files:**
- Modify: `MacEverything/Core/SearchEngine.h` (add private declaration)
- Modify: `MacEverything/Core/SearchEngine.cpp` (add implementation, replace 4 call sites)

- [ ] **Step 1: Add declaration to SearchEngine.h**

Add in the `private:` section:

```cpp
    /// Intersect posting lists from a trigram index for a given keyword.
    /// Returns sorted candidate indices, or empty if any trigram is missing.
    /// Sets allFound to false if any trigram is not in the index.
    static std::vector<uint32_t> intersectPostingLists(
        const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
        const std::string& keyword,
        bool& allFound);
```

- [ ] **Step 2: Add implementation to SearchEngine.cpp**

Add after `namePriority`:

```cpp
std::vector<uint32_t> SearchEngine::intersectPostingLists(
    const std::unordered_map<Trigram, std::vector<uint32_t>>& index,
    const std::string& keyword,
    bool& allFound) {
    allFound = true;
    auto keyTrigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(keyTrigrams.begin(), keyTrigrams.end());
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const std::vector<uint32_t>*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}
```

- [ ] **Step 3: Replace 4 inline intersection blocks**

**Call site 1 — Name trigram candidates (lines 402-436):**

Replace lines 402-442 with:
```cpp
        bool nameAllFound = false;
        trigramCandidates = intersectPostingLists(nameTrigramIndex_, lowerKey, nameAllFound);
        if (!nameAllFound) {
            useTrigramIndex = false;
        } else if (trigramCandidates.size() > totalSize / 67) { // ~1.5%
            trigramCandidates.clear();
            useTrigramIndex = false;
        }
```

**Call site 2 — Slash-split path trigram intersection (lines 525-546):**

Replace with:
```cpp
                if (pathPartUsable) {
                    candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, pathPart, pathFound);
                }
```

**Call site 3 — Slash-split name trigram intersection (lines 552-573):**

Replace with:
```cpp
                if (namePartUsable && !nameTrigramIndex_.empty()) {
                    nameRecCandidates = intersectPostingLists(nameTrigramIndex_, namePart, nameFound);
                }
```

**Call site 4 — Path trigram Phase 2 intersection (lines 718-750):**

Replace with:
```cpp
            bool pathAllFound = false;
            std::vector<uint32_t> candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, lowerKey, pathAllFound);
```

- [ ] **Step 4: Compile and run tests**

```bash
clang++ -std=c++20 -O2 -framework CoreServices -framework CoreFoundation MacEverything/Core/*.cpp test_all.cpp -o test_all
./test_all --part 3b --part 44 --part 48
```

Expected: Same pass/fail counts as baseline.

- [ ] **Step 5: Commit**

```bash
git add MacEverything/Core/SearchEngine.h MacEverything/Core/SearchEngine.cpp
git commit -m "refactor: extract intersectPostingLists(), eliminate 4 duplicated intersection blocks"
```

---

### Task 4: Extract `buildFullPath()` and `scoreRecord()` Helpers

These eliminate duplicated full-path construction (buffer management, memcpy, SIMD lowercase) and the match-score-and-push pattern.

**Files:**
- Modify: `MacEverything/Core/SearchEngine.h` (add private declarations)
- Modify: `MacEverything/Core/SearchEngine.cpp` (add implementations, replace call sites)

- [ ] **Step 1: Add declarations to SearchEngine.h**

```cpp
    /// Build full path in a reusable buffer: path + '/' + name.
    /// Lowercases the path portion via SIMD. Returns the full path length.
    /// Buffer is resized if needed (with 2x growth).
    static size_t buildFullPathBuf(std::vector<char>& buf,
                                   const char* pathData, uint16_t pathLen,
                                   const char* nameData, uint16_t nameLen);
```

- [ ] **Step 2: Add implementation to SearchEngine.cpp**

```cpp
size_t SearchEngine::buildFullPathBuf(std::vector<char>& buf,
                                      const char* pathData, uint16_t pathLen,
                                      const char* nameData, uint16_t nameLen) {
    size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
    if (buf.size() < fullLen) buf.resize(fullLen * 2);
    memcpy(buf.data(), pathData, pathLen);
    buf[pathLen] = '/';
    memcpy(buf.data() + pathLen + 1, nameData, nameLen);
    me::simdToLowerAscii(buf.data(), pathLen); // lowercase path; name already lower
    return fullLen;
}
```

- [ ] **Step 3: Replace full-path construction blocks across query()**

Replace all occurrences of the pattern:
```cpp
size_t fullLen = static_cast<size_t>(pl) + 1 + nl;
if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
memcpy(pathBuf.data(), pd, pl);
pathBuf[pl] = '/';
memcpy(pathBuf.data() + pl + 1, nd, nl);
me::simdToLowerAscii(pathBuf.data(), pl);
```

With:
```cpp
size_t fullLen = buildFullPathBuf(pathBuf, pd, pl, nd, nl);
```

This appears in:
1. Slash-split both-indexes loop (line ~597-602)
2. Slash-split path-only supplement loop (line ~636-642)
3. Slash-split path-only-index branch (line ~670-675)
4. Slash-split name-only-index branch (line ~695-700)
5. Trigram Phase 2 linear scan fallback (line ~843-848)
6. Full linear scan (line ~912-917)

- [ ] **Step 4: Compile and run tests**

```bash
clang++ -std=c++20 -O2 -framework CoreServices -framework CoreFoundation MacEverything/Core/*.cpp test_all.cpp -o test_all
./test_all --part 3b --part 44 --part 48
```

Expected: Same pass/fail counts as baseline.

- [ ] **Step 5: Commit**

```bash
git add MacEverything/Core/SearchEngine.h MacEverything/Core/SearchEngine.cpp
git commit -m "refactor: extract buildFullPathBuf() helper, eliminate 6 duplicated path-build blocks"
```

---

### Task 5: Extract Strategy Methods — Phase 1 (Name Trigram + Slash Split)

Extract the two largest strategy blocks into private methods. The `query()` method will call these instead of inlining the logic.

**Files:**
- Modify: `MacEverything/Core/SearchEngine.h` (add private declarations)
- Modify: `MacEverything/Core/SearchEngine.cpp` (move code into new methods)

- [ ] **Step 1: Define the Match struct and shared types at class scope**

Move the `Match` struct from inside `query()` to a private definition in SearchEngine.h:

```cpp
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };
```

- [ ] **Step 2: Add strategy method declarations to SearchEngine.h**

```cpp
    /// Phase 2 strategy: slash-split query using path + name trigram indexes.
    /// Appends results to `merged`. Uses `isCandidate` for dedup with Phase 1.
    void querySlashSplit(const std::string& lowerKey,
                         size_t totalSize, uint64_t myGen,
                         ReusableBitmap& isCandidate,
                         std::vector<Match>& merged) const;

    /// Phase 2 strategy: path trigram lookup for non-slash queries.
    /// Appends path-only matches to `merged`.
    void queryPathTrigram(const std::string& lowerKey,
                          size_t totalSize, uint64_t myGen,
                          const std::vector<uint32_t>& trigramCandidates,
                          std::vector<Match>& merged) const;
```

Note: `ReusableBitmap` is defined in the anonymous namespace in SearchEngine.cpp — we need to forward-declare or move it. Since it's an implementation detail, we'll use a forward-declared opaque type or keep the method definition in SearchEngine.cpp with full access.

Actually, since `ReusableBitmap` is in an anonymous namespace and `Match` is a private struct, the strategy methods must be defined in SearchEngine.cpp (they already have access). The declarations in .h just need the `Match` type visible and `ReusableBitmap` can be passed as a reference to the thread_local instance.

Revised approach — keep `ReusableBitmap` as-is (anonymous namespace), declare strategy methods that accept the bitmap by reference:

In SearchEngine.h, forward declare in the private section:
```cpp
    // Forward ref: ReusableBitmap defined in SearchEngine.cpp anonymous namespace
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };

    // Phase 2 strategies (defined in SearchEngine.cpp, access ReusableBitmap via param)
    void querySlashSplit(const std::string& lowerKey,
                         size_t totalSize, uint64_t myGen,
                         std::vector<Match>& merged) const;

    void queryPathTrigram(const std::string& lowerKey,
                          size_t totalSize, uint64_t myGen,
                          const std::vector<uint32_t>& trigramCandidates,
                          std::vector<Match>& merged) const;

    /// Full linear scan for glob patterns and short keywords.
    void queryLinearScan(const std::string& lowerKey,
                         bool useGlob, size_t totalSize, uint64_t myGen,
                         std::vector<Match>& merged) const;

    /// Linear scan fallback for trigram Phase 2 path matches.
    void queryLinearScanPath(const std::string& lowerKey,
                             size_t totalSize, uint64_t myGen,
                             const std::vector<uint32_t>& trigramCandidates,
                             std::vector<Match>& merged) const;
```

- [ ] **Step 3: Extract `querySlashSplit()` from query()**

Move lines 497-711 of the current query() into `SearchEngine::querySlashSplit()`:

```cpp
void SearchEngine::querySlashSplit(const std::string& lowerKey,
                                   size_t totalSize, uint64_t myGen,
                                   std::vector<Match>& merged) const {
    std::vector<char> pathBuf;
    size_t lastSlash = lowerKey.rfind('/');
    std::string pathPart = lowerKey.substr(0, lastSlash);
    std::string namePart = lowerKey.substr(lastSlash + 1);

    if (pathPart.empty() && lowerKey.size() >= 3) {
        pathPart = lowerKey;
    }

    bool pathPartUsable = pathPart.size() >= 3;
    bool namePartUsable = namePart.size() >= 3;

    if (!pathPartUsable && !namePartUsable) return; // caller falls through to linear

    // Step 1: path trigram intersection
    std::vector<uint32_t> candidatePathIdxs;
    bool pathFound = true;
    if (pathPartUsable) {
        candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, pathPart, pathFound);
    }

    // Step 2: name trigram intersection
    std::vector<uint32_t> nameRecCandidates;
    bool nameFound = true;
    if (namePartUsable && !nameTrigramIndex_.empty()) {
        nameRecCandidates = intersectPostingLists(nameTrigramIndex_, namePart, nameFound);
    }

    // Step 3: Combine and verify
    auto& isCandidate = threadLocalBitmap();
    // Note: isCandidate is already prepared by caller with trigramCandidates

    if (pathPartUsable && pathFound && namePartUsable && nameFound) {
        // [existing both-indexes logic with buildFullPathBuf and namePriority calls]
        // ... (move the existing code block here, using the new helpers)
    } else if (pathPartUsable && pathFound && !candidatePathIdxs.empty()) {
        // [existing path-only logic]
    } else if (namePartUsable && nameFound && !nameRecCandidates.empty()) {
        // [existing name-only logic]
    }
}
```

The actual implementation is a direct cut-paste of lines 497-711, replacing the inline priority and intersection code with the helpers from Tasks 2-4.

- [ ] **Step 4: Extract `queryPathTrigram()` from query()**

Move lines 716-788 into `SearchEngine::queryPathTrigram()`:

```cpp
void SearchEngine::queryPathTrigram(const std::string& lowerKey,
                                    size_t totalSize, uint64_t myGen,
                                    const std::vector<uint32_t>& trigramCandidates,
                                    std::vector<Match>& merged) const {
    bool pathAllFound = false;
    std::vector<uint32_t> candidatePathIdxs = intersectPostingLists(pathTrigramIndex_, lowerKey, pathAllFound);
    if (!pathAllFound || candidatePathIdxs.empty()) return;

    auto& isCandidate = threadLocalBitmap();
    isCandidate.prepare(totalSize);
    isCandidate.populateFrom(trigramCandidates);

    for (uint32_t pi : candidatePathIdxs) {
        if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return;
        if (pi >= pathIdxToRecords_.size()) continue;

        std::string rPath = pathPool_.str(pi);
        std::string lowerPath = me::toLower(rPath);
        if (lowerPath.find(lowerKey) == std::string::npos) continue;

        const auto& recIndices = pathIdxToRecords_[pi];
        for (uint32_t idx : recIndices) {
            if (records_[idx].type == 0) continue;
            if (isCandidate.test(idx)) continue;
            const char* nd = namePool_.data(idx);
            uint16_t nl = namePool_.length(idx);
            uint8_t priority = me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())
                ? namePriority(nd, nl, lowerKey.data(), lowerKey.size()) : 3;
            uint32_t pLen = static_cast<uint32_t>(rPath.size() + 1 + nl);
            merged.push_back({idx, priority, pLen});
        }
    }
}
```

- [ ] **Step 5: Extract `queryLinearScanPath()` from query()**

Move lines 790-861 (the trigram Phase 2 fallback parallel scan) into its own method.

- [ ] **Step 6: Extract `queryLinearScan()` from query()**

Move lines 865-949 (the full linear scan for glob/short keywords) into its own method.

- [ ] **Step 7: Rewrite query() as a dispatcher**

The new `query()` should be approximately:

```cpp
std::vector<uint32_t> SearchEngine::query(const std::string& keyword, uint32_t maxResults,
                                          bool useTrigram, QueryTimingInfo& timing) const {
    uint64_t myGen = queryGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (keyword.empty()) return {};

    auto queryStart = std::chrono::steady_clock::now();
    std::string lowerKey = me::toLower(keyword);
    bool useGlob = isGlobPattern(lowerKey);
    bool hasSlash = lowerKey.find('/') != std::string::npos;

    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (records_.empty()) return {};
    size_t totalSize = records_.size();

    auto beforeTrigram = afterLock, afterTrigram = afterLock;
    auto afterPhase1 = afterLock;
    auto beforePhase2 = afterLock, afterPhase2 = afterLock;
    size_t phase1Results = 0;

    std::vector<uint32_t> trigramCandidates;
    bool useTrigramIndex = useTrigram && !useGlob && lowerKey.size() >= 3 && !nameTrigramIndex_.empty();
    bool useSlashSplit = false;

    // --- Phase 0: Build name trigram candidates ---
    if (useTrigramIndex) {
        beforeTrigram = std::chrono::steady_clock::now();
        bool allFound = false;
        trigramCandidates = intersectPostingLists(nameTrigramIndex_, lowerKey, allFound);
        if (!allFound || trigramCandidates.size() > totalSize / 67) {
            trigramCandidates.clear();
            useTrigramIndex = false;
        }
        afterTrigram = std::chrono::steady_clock::now();
    }

    std::vector<Match> merged;

    if (useTrigramIndex) {
        // --- Phase 1: Verify name trigram candidates ---
        for (size_t ci = 0; ci < trigramCandidates.size(); ci++) {
            if ((ci & 1023) == 0 && queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};
            uint32_t idx = trigramCandidates[ci];
            if (records_[idx].type == 0) continue;
            const char* nd = namePool_.data(idx);
            uint16_t nl = namePool_.length(idx);
            if (me::simdContains(nd, nl, lowerKey.data(), lowerKey.size())) {
                uint8_t priority = namePriority(nd, nl, lowerKey.data(), lowerKey.size());
                uint32_t pLen = static_cast<uint32_t>(pathPool_.length(pathIndices_[idx]) + 1 + nl);
                merged.push_back({idx, priority, pLen});
            }
        }
        afterPhase1 = std::chrono::steady_clock::now();
        phase1Results = merged.size();

        // --- Phase 2: Path-based supplemental matches ---
        if (maxResults > 0 && merged.size() >= maxResults) {
            beforePhase2 = afterPhase2 = afterPhase1;
        } else {
            beforePhase2 = std::chrono::steady_clock::now();
            useSlashSplit = !pathTrigramIndex_.empty() && hasSlash;
            bool usePathTri = !pathTrigramIndex_.empty() && !hasSlash && lowerKey.size() >= 3;

            if (useSlashSplit) {
                querySlashSplit(lowerKey, totalSize, myGen, merged);
            } else if (usePathTri) {
                queryPathTrigram(lowerKey, totalSize, myGen, trigramCandidates, merged);
            } else {
                queryLinearScanPath(lowerKey, totalSize, myGen, trigramCandidates, merged);
            }
            afterPhase2 = std::chrono::steady_clock::now();
        }
    } else {
        // --- Non-trigram path: full linear scan ---
        beforePhase2 = std::chrono::steady_clock::now();
        queryLinearScan(lowerKey, useGlob, totalSize, myGen, merged);
        afterPhase2 = std::chrono::steady_clock::now();
    }

    if (queryGeneration_.load(std::memory_order_relaxed) != myGen) return {};

    // Sort and collect results
    auto beforeUnlock = std::chrono::steady_clock::now();
    lock.unlock();

    auto beforeSort = std::chrono::steady_clock::now();
    auto cmp = [](const Match& a, const Match& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.pathLen < b.pathLen;
    };

    size_t resultCount = merged.size();
    if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;

    if (resultCount < merged.size()) {
        std::partial_sort(merged.begin(), merged.begin() + resultCount, merged.end(), cmp);
    } else {
        std::sort(merged.begin(), merged.end(), cmp);
    }
    auto afterSort = std::chrono::steady_clock::now();

    std::vector<uint32_t> result;
    result.reserve(resultCount);
    for (size_t i = 0; i < resultCount; i++) {
        result.push_back(merged[i].idx);
    }

    // Timing instrumentation
    auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
    timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
    timing.lockWaitMs = toMs(afterLock - beforeLock);
    timing.lockHeldMs = toMs(beforeUnlock - afterLock);
    timing.sortMs = toMs(afterSort - beforeSort);
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = toMs(afterPhase1 - afterTrigram);
    timing.phase2Ms = toMs(afterPhase2 - beforePhase2);
    timing.totalRecords = totalSize;
    timing.candidates = trigramCandidates.size();
    timing.nameMatches = phase1Results;
    timing.pathMatches = merged.size() > phase1Results ? merged.size() - phase1Results : 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex;
    timing.searchPath = useTrigramIndex ? (useSlashSplit ? "trigram-split" : "trigram") : "linear";

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        // ... existing logging ...
    }
    return result;
}
```

- [ ] **Step 8: Compile and run tests**

```bash
clang++ -std=c++20 -O2 -framework CoreServices -framework CoreFoundation MacEverything/Core/*.cpp test_all.cpp -o test_all
./test_all --part 3b --part 44 --part 48
```

Expected: All tests PASS with identical counts.

- [ ] **Step 9: Run full fast test suite**

```bash
./test_all --fast
```

Expected: All PASS.

- [ ] **Step 10: Commit**

```bash
git add MacEverything/Core/SearchEngine.h MacEverything/Core/SearchEngine.cpp
git commit -m "refactor: extract 4 strategy methods, reduce query() from ~640 to ~80 lines"
```

---

### Task 6: Final Verification — Build, Package, and Functional Test

**Files:**
- Build: entire project
- Test: HTTP endpoint

- [ ] **Step 1: Build release**

```bash
cd /Users/wujian/data/project/mac_everything
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -project MacEverything.xcodeproj -scheme MacEverything -configuration Release build SYMROOT=build
```

- [ ] **Step 2: Package DMG**

```bash
hdiutil create -volname MacEverything -srcfolder build/Release/MacEverything.app -ov -format UDZO /Users/wujian/data/project/mac_everything/MacEverything.dmg
```

- [ ] **Step 3: Launch app minimized**

```bash
open build/Release/MacEverything.app --args --minimized
```

- [ ] **Step 4: Functional verification via HTTP**

```bash
# Test trigram path
curl -s 'http://localhost:19860/api/search?q=SearchEngine&limit=5' | python3 -m json.tool

# Test slash-split path
curl -s 'http://localhost:19860/api/search?q=Core/Search&limit=5' | python3 -m json.tool

# Test short keyword (linear scan)
curl -s 'http://localhost:19860/api/search?q=te&limit=5' | python3 -m json.tool

# Test glob
curl -s 'http://localhost:19860/api/search?q=*.cpp&limit=5' | python3 -m json.tool

# Test absolute path
curl -s 'http://localhost:19860/api/search?q=/usr/local&limit=5' | python3 -m json.tool
```

Expected: All queries return valid JSON with results.

- [ ] **Step 5: Verify timing info is present**

```bash
curl -s 'http://localhost:19860/api/search?q=SearchEngine&limit=5' | python3 -c "import sys,json; d=json.load(sys.stdin); print('searchPath:', d.get('timing',{}).get('searchPath','MISSING'))"
```

Expected: `searchPath: trigram`

---

### Task 7: Write Changelog

**Files:**
- Create: `docs/changelog/090-query-refactor.md`

- [ ] **Step 1: Write changelog**

```markdown
# 090 — SearchEngine::query() 分支复杂度重构

## 变更类型
纯重构，零行为变更。

## 背景
`query()` 函数约 640 行，嵌套深度达 7 层，包含：
- 7 处重复的优先级计算代码块
- 4 处重复的 trigram posting list 交集代码块
- 6 处重复的全路径构建代码块

## 实施

### 提取的辅助函数
1. **`namePriority()`** — 统一匹配优先级计算（exact=0, starts-with=1, contains=2）
2. **`intersectPostingLists()`** — 通用 trigram posting list 交集，支持 nameTrigramIndex_ 和 pathTrigramIndex_
3. **`buildFullPathBuf()`** — 复用缓冲区构建小写全路径

### 提取的策略方法
4. **`querySlashSplit()`** — 处理含 `/` 的查询（路径+文件名分割搜索）
5. **`queryPathTrigram()`** — 使用路径 trigram 索引搜索
6. **`queryLinearScanPath()`** — trigram Phase 2 的线性扫描兜底
7. **`queryLinearScan()`** — glob 和短关键词的全量线性扫描

### 结果
- `query()` 从 ~640 行缩减到 ~80 行调度器
- 最大嵌套深度从 7 层降到 3 层
- 所有现有测试（test_slash_query 10 项、test_path_search 8 项、test_query_perf 7 场景）通过

## 测试验证
- `./test_all --part 3b --part 44 --part 48` 全部通过
- `./test_all --fast` 全部通过
- HTTP API 功能验证通过（trigram、slash-split、linear、glob 路径均正常）
```

- [ ] **Step 2: Commit changelog**

```bash
git add docs/changelog/090-query-refactor.md
git commit -m "docs: add changelog for query refactor (090)"
```
