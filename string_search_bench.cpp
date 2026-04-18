#include <algorithm>
#include <arm_neon.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t DATA_SIZE   = 5ULL * 1024 * 1024 * 1024;
static constexpr int    NUM_INSERTS = 1000;
static constexpr int    SEED        = 42;
static constexpr int    NUM_RUNS    = 3;

struct BenchResult {
    std::string name;
    size_t      matches;
    double      min_ms, max_ms, avg_ms;
    double      avg_gbs;
};

// Single-threaded search function signature: returns list of match positions
using SearchFn = std::function<std::vector<size_t>(const uint8_t*, size_t,
                                                    const uint8_t*, size_t)>;

// ─────────────────────────────────────────────────────────────────────────────
// Generic multi-thread wrapper: splits data into chunks, runs any SearchFn
// on each chunk in a separate thread, merges & deduplicates results.
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> multithread_wrapper(SearchFn inner,
                                                const uint8_t* data, size_t data_len,
                                                const uint8_t* needle, size_t needle_len) {
    unsigned num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (data_len < 1024 * 1024) num_threads = 1; // don't bother for tiny data

    size_t chunk_base = data_len / num_threads;
    size_t overlap = (needle_len > 1) ? needle_len - 1 : 0;

    std::vector<std::thread> threads;
    std::vector<std::vector<size_t>> thread_results(num_threads);

    for (unsigned t = 0; t < num_threads; t++) {
        size_t start = t * chunk_base;
        size_t end = (t == num_threads - 1) ? data_len : (t + 1) * chunk_base + overlap;
        if (end > data_len) end = data_len;
        size_t len = end - start;

        threads.emplace_back([&inner, data, start, len, needle, needle_len,
                              &out = thread_results[t]]() {
            out = inner(data + start, len, needle, needle_len);
            // Adjust positions from chunk-local to global
            for (auto& pos : out) pos += start;
        });
    }

    for (auto& th : threads) th.join();

    std::vector<size_t> merged;
    for (auto& v : thread_results)
        merged.insert(merged.end(), v.begin(), v.end());

    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    return merged;
}

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark runner: runs a search function NUM_RUNS times, collects stats
// ─────────────────────────────────────────────────────────────────────────────

static BenchResult run_bench(const std::string& name, SearchFn fn,
                             const uint8_t* data, size_t data_len,
                             const uint8_t* needle, size_t needle_len,
                             int num_runs) {
    std::cout << "  " << name << ": " << std::flush;

    std::vector<double> times;
    size_t match_count = 0;

    for (int r = 0; r < num_runs; r++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto results = fn(data, data_len, needle, needle_len);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        times.push_back(ms);
        match_count = results.size();
        std::cout << std::fixed << std::setprecision(0) << ms << "ms ";
        std::cout.flush();
    }
    std::cout << "\n";

    double min_ms = *std::min_element(times.begin(), times.end());
    double max_ms = *std::max_element(times.begin(), times.end());
    double avg_ms = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    double avg_gbs = (data_len / (1024.0 * 1024 * 1024)) / (avg_ms / 1000.0);

    return {name, match_count, min_ms, max_ms, avg_ms, avg_gbs};
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. std::string::find
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_std_find(const uint8_t* data, size_t data_len,
                                           const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    std::string_view haystack(reinterpret_cast<const char*>(data), data_len);
    std::string_view pat(reinterpret_cast<const char*>(needle), needle_len);
    size_t pos = 0;
    while ((pos = haystack.find(pat, pos)) != std::string_view::npos) {
        positions.push_back(pos);
        pos++;
    }
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. memmem
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_memmem_fn(const uint8_t* data, size_t data_len,
                                             const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    const uint8_t* p = data;
    size_t remaining = data_len;
    while (remaining >= needle_len) {
        void* found = memmem(p, remaining, needle, needle_len);
        if (!found) break;
        size_t offset = static_cast<const uint8_t*>(found) - data;
        positions.push_back(offset);
        p = static_cast<const uint8_t*>(found) + 1;
        remaining = data_len - (p - data);
    }
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. KMP
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_kmp(const uint8_t* data, size_t data_len,
                                      const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    if (needle_len == 0 || needle_len > data_len) return positions;
    std::vector<int> fail(needle_len, 0);
    for (size_t i = 1; i < needle_len; i++) {
        int j = fail[i - 1];
        while (j > 0 && needle[i] != needle[j]) j = fail[j - 1];
        if (needle[i] == needle[j]) j++;
        fail[i] = j;
    }
    size_t j = 0;
    for (size_t i = 0; i < data_len; i++) {
        while (j > 0 && data[i] != needle[j]) j = fail[j - 1];
        if (data[i] == needle[j]) j++;
        if (j == needle_len) {
            positions.push_back(i - needle_len + 1);
            j = fail[j - 1];
        }
    }
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Boyer-Moore-Horspool
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_horspool(const uint8_t* data, size_t data_len,
                                           const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    if (needle_len == 0 || needle_len > data_len) return positions;
    size_t shift[256];
    for (int i = 0; i < 256; i++) shift[i] = needle_len;
    for (size_t i = 0; i < needle_len - 1; i++)
        shift[needle[i]] = needle_len - 1 - i;

    size_t i = 0;
    while (i <= data_len - needle_len) {
        size_t j = needle_len - 1;
        while (data[i + j] == needle[j]) {
            if (j == 0) { positions.push_back(i); break; }
            j--;
        }
        i += shift[data[i + needle_len - 1]];
    }
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Rabin-Karp
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_rabin_karp(const uint8_t* data, size_t data_len,
                                             const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    if (needle_len == 0 || needle_len > data_len) return positions;

    constexpr uint64_t BASE = 256;
    constexpr uint64_t MOD  = (1ULL << 61) - 1;
    auto mod_mul = [](uint64_t a, uint64_t b, uint64_t m) -> uint64_t {
        __uint128_t r = (__uint128_t)a * b;
        return (uint64_t)(r % m);
    };

    uint64_t needle_hash = 0, window_hash = 0, high_pow = 1;
    for (size_t i = 0; i < needle_len; i++) {
        needle_hash = (mod_mul(needle_hash, BASE, MOD) + needle[i]) % MOD;
        window_hash = (mod_mul(window_hash, BASE, MOD) + data[i]) % MOD;
        if (i > 0) high_pow = mod_mul(high_pow, BASE, MOD);
    }
    for (size_t i = 0; i <= data_len - needle_len; i++) {
        if (window_hash == needle_hash &&
            memcmp(data + i, needle, needle_len) == 0)
            positions.push_back(i);
        if (i < data_len - needle_len) {
            window_hash = (window_hash + MOD - mod_mul(data[i], high_pow, MOD)) % MOD;
            window_hash = (mod_mul(window_hash, BASE, MOD) + data[i + needle_len]) % MOD;
        }
    }
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// NEON helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline uint16_t neon_movemask(uint8x16_t input) {
    static const uint8_t mask_bytes[16] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128
    };
    uint8x16_t bit_mask = vld1q_u8(mask_bytes);
    uint8x16_t masked = vandq_u8(input, bit_mask);
    uint8_t lo = vaddv_u8(vget_low_u8(masked));
    uint8_t hi = vaddv_u8(vget_high_u8(masked));
    return lo | ((uint16_t)hi << 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. NEON first-last byte
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_neon_firstlast(const uint8_t* data, size_t data_len,
                                                  const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    if (needle_len == 0 || needle_len > data_len) return positions;
    if (needle_len == 1) {
        for (size_t i = 0; i < data_len; i++)
            if (data[i] == needle[0]) positions.push_back(i);
        return positions;
    }

    uint8x16_t v_first = vdupq_n_u8(needle[0]);
    uint8x16_t v_last  = vdupq_n_u8(needle[needle_len - 1]);
    size_t end = data_len - needle_len + 1;
    size_t i = 0;

    for (; i + 16 <= end; i += 16) {
        uint8x16_t eq_first = vceqq_u8(vld1q_u8(data + i), v_first);
        uint8x16_t eq_last  = vceqq_u8(vld1q_u8(data + i + needle_len - 1), v_last);
        uint8x16_t match    = vandq_u8(eq_first, eq_last);
        if (vmaxvq_u8(match) == 0) continue;
        uint16_t mask = neon_movemask(match);
        while (mask) {
            int pos = __builtin_ctz(mask);
            if (memcmp(data + i + pos + 1, needle + 1, needle_len - 2) == 0)
                positions.push_back(i + pos);
            mask &= mask - 1;
        }
    }
    for (; i < end; i++)
        if (memcmp(data + i, needle, needle_len) == 0)
            positions.push_back(i);
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. NEON first-last byte (2x unroll)
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<size_t> search_neon_2x(const uint8_t* data, size_t data_len,
                                           const uint8_t* needle, size_t needle_len) {
    std::vector<size_t> positions;
    if (needle_len == 0 || needle_len > data_len) return positions;
    if (needle_len == 1) {
        for (size_t i = 0; i < data_len; i++)
            if (data[i] == needle[0]) positions.push_back(i);
        return positions;
    }

    uint8x16_t v_first = vdupq_n_u8(needle[0]);
    uint8x16_t v_last  = vdupq_n_u8(needle[needle_len - 1]);
    size_t end = data_len - needle_len + 1;
    size_t i = 0;

    for (; i + 32 <= end; i += 32) {
        uint8x16_t a_match = vandq_u8(vceqq_u8(vld1q_u8(data + i), v_first),
                                       vceqq_u8(vld1q_u8(data + i + needle_len - 1), v_last));
        uint8x16_t b_match = vandq_u8(vceqq_u8(vld1q_u8(data + i + 16), v_first),
                                       vceqq_u8(vld1q_u8(data + i + 16 + needle_len - 1), v_last));
        if (vmaxvq_u8(vorrq_u8(a_match, b_match)) == 0) continue;

        if (vmaxvq_u8(a_match) != 0) {
            uint16_t mask = neon_movemask(a_match);
            while (mask) {
                int pos = __builtin_ctz(mask);
                if (memcmp(data + i + pos + 1, needle + 1, needle_len - 2) == 0)
                    positions.push_back(i + pos);
                mask &= mask - 1;
            }
        }
        if (vmaxvq_u8(b_match) != 0) {
            uint16_t mask = neon_movemask(b_match);
            while (mask) {
                int pos = __builtin_ctz(mask);
                if (memcmp(data + i + 16 + pos + 1, needle + 1, needle_len - 2) == 0)
                    positions.push_back(i + 16 + pos);
                mask &= mask - 1;
            }
        }
    }
    for (; i + 16 <= end; i += 16) {
        uint8x16_t match = vandq_u8(vceqq_u8(vld1q_u8(data + i), v_first),
                                     vceqq_u8(vld1q_u8(data + i + needle_len - 1), v_last));
        if (vmaxvq_u8(match) == 0) continue;
        uint16_t mask = neon_movemask(match);
        while (mask) {
            int pos = __builtin_ctz(mask);
            if (memcmp(data + i + pos + 1, needle + 1, needle_len - 2) == 0)
                positions.push_back(i + pos);
            mask &= mask - 1;
        }
    }
    for (; i < end; i++)
        if (memcmp(data + i, needle, needle_len) == 0)
            positions.push_back(i);
    return positions;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* query = (argc > 1) ? argv[1] : "hello";
    size_t query_len = strlen(query);
    unsigned hw_threads = std::thread::hardware_concurrency();

    // ── Allocate & fill ──────────────────────────────────────────────────
    std::cout << "Allocating " << DATA_SIZE / (1024.0 * 1024 * 1024)
              << " GB..." << std::flush;
    uint8_t* data = static_cast<uint8_t*>(malloc(DATA_SIZE));
    if (!data) { std::cerr << " FAILED!\n"; return 1; }
    std::cout << " OK\n";

    std::cout << "Filling random data..." << std::flush;
    {
        std::mt19937_64 rng(SEED);
        uint64_t* p64 = reinterpret_cast<uint64_t*>(data);
        size_t n64 = DATA_SIZE / 8;
        for (size_t i = 0; i < n64; i++) {
            uint64_t r = rng();
            uint64_t val = 0;
            for (int b = 0; b < 8; b++) {
                uint8_t ch = 'a' + ((r >> (b * 8)) & 0xFF) % 26;
                val |= (uint64_t)ch << (b * 8);
            }
            p64[i] = val;
        }
        for (size_t i = n64 * 8; i < DATA_SIZE; i++)
            data[i] = 'a' + (rng() % 26);
    }
    std::cout << " OK\n";

    std::cout << "Inserting " << NUM_INSERTS << " copies of \"" << query << "\"..." << std::flush;
    {
        std::mt19937_64 rng(SEED + 1);
        std::uniform_int_distribution<size_t> dist(0, DATA_SIZE - query_len);
        for (int i = 0; i < NUM_INSERTS; i++)
            memcpy(data + dist(rng), query, query_len);
    }
    std::cout << " OK\n";

    // ── Define algorithms ────────────────────────────────────────────────
    struct AlgoDef {
        std::string name;
        SearchFn    fn;
    };

    // Single-threaded algorithms
    std::vector<AlgoDef> single_algos = {
        {"std::string::find",     search_std_find},
        {"memmem",                search_memmem_fn},
        {"KMP",                   search_kmp},
        {"Boyer-Moore-Horspool",  search_horspool},
        {"Rabin-Karp",            search_rabin_karp},
        {"NEON first-last",       search_neon_firstlast},
        {"NEON first-last (2x)",  search_neon_2x},
    };

    // Multi-threaded variants (wrap each single-threaded algo)
    std::vector<AlgoDef> multi_algos;
    // Only multi-thread the interesting ones (skip Rabin-Karp which is very slow)
    std::vector<std::string> mt_candidates = {
        "std::string::find", "memmem", "KMP",
        "Boyer-Moore-Horspool",
        "NEON first-last", "NEON first-last (2x)"
    };
    for (auto& sa : single_algos) {
        for (auto& mc : mt_candidates) {
            if (sa.name == mc) {
                SearchFn inner = sa.fn;
                multi_algos.push_back({
                    sa.name + " [MT]",
                    [inner](const uint8_t* d, size_t dl, const uint8_t* n, size_t nl) {
                        return multithread_wrapper(inner, d, dl, n, nl);
                    }
                });
                break;
            }
        }
    }

    // ── Run benchmarks ───────────────────────────────────────────────────
    auto run_section = [&](const std::string& title,
                           std::vector<AlgoDef>& algos) -> std::vector<BenchResult> {
        std::cout << "\n=== " << title << " (query=\"" << query
                  << "\", " << NUM_RUNS << " runs) ===\n";
        std::cout << "Platform: Apple M3 Pro (ARM64, NEON), "
                  << hw_threads << " threads\n\n";

        std::vector<BenchResult> results;
        for (auto& a : algos) {
            auto r = run_bench(a.name, a.fn, data, DATA_SIZE,
                              reinterpret_cast<const uint8_t*>(query), query_len,
                              NUM_RUNS);
            results.push_back(r);
        }

        // Print table
        std::cout << "\n" << std::left << std::setw(30) << "Algorithm"
                  << std::right << std::setw(8) << "Matches"
                  << std::setw(10) << "Min(ms)"
                  << std::setw(10) << "Avg(ms)"
                  << std::setw(10) << "Max(ms)"
                  << std::setw(14) << "Avg(GB/s)"
                  << "\n" << std::string(82, '-') << "\n";

        for (auto& r : results) {
            std::cout << std::left << std::setw(30) << r.name
                      << std::right << std::setw(8) << r.matches
                      << std::setw(10) << std::fixed << std::setprecision(1) << r.min_ms
                      << std::setw(10) << r.avg_ms
                      << std::setw(10) << r.max_ms
                      << std::setw(14) << std::setprecision(2) << r.avg_gbs
                      << "\n";
        }

        // Verify
        bool ok = true;
        for (size_t i = 1; i < results.size(); i++)
            if (results[i].matches != results[0].matches) ok = false;
        std::cout << "\n" << (ok ? "All match counts agree." : "WARNING: mismatch!") << "\n";
        return results;
    };

    auto st_results = run_section("Single-threaded", single_algos);
    auto mt_results = run_section("Multi-threaded (" + std::to_string(hw_threads) + " threads)",
                                   multi_algos);

    // ── Summary ──────────────────────────────────────────────────────────
    std::cout << "\n=== Summary ===\n\n";

    // Combine all results
    std::vector<BenchResult> all_results;
    all_results.insert(all_results.end(), st_results.begin(), st_results.end());
    all_results.insert(all_results.end(), mt_results.begin(), mt_results.end());

    // Sort by throughput descending
    std::sort(all_results.begin(), all_results.end(),
              [](const BenchResult& a, const BenchResult& b) {
                  return a.avg_gbs > b.avg_gbs;
              });

    std::cout << std::left << std::setw(30) << "Algorithm"
              << std::right << std::setw(12) << "Avg(ms)"
              << std::setw(14) << "Avg(GB/s)"
              << std::setw(10) << "Speedup"
              << "\n" << std::string(66, '-') << "\n";

    double baseline_ms = 0;
    // Find std::string::find as baseline
    for (auto& r : all_results)
        if (r.name == "std::string::find") { baseline_ms = r.avg_ms; break; }

    for (auto& r : all_results) {
        std::cout << std::left << std::setw(30) << r.name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(1) << r.avg_ms
                  << std::setw(14) << std::setprecision(2) << r.avg_gbs
                  << std::setw(9) << std::setprecision(1) << (baseline_ms / r.avg_ms) << "x"
                  << "\n";
    }

    std::cout << "\nFastest: " << all_results[0].name << " ("
              << std::fixed << std::setprecision(2) << all_results[0].avg_gbs << " GB/s, "
              << std::setprecision(1) << (baseline_ms / all_results[0].avg_ms) << "x vs baseline)\n";

    free(data);
    return 0;
}
