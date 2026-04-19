#pragma once
#include "test_helpers.h"
#include "../MacEverything/Core/SIMDSearch.h"
#include <iostream>
#include <cstring>
#include <vector>

inline void runSIMDBatchFilterTests() {
    std::cout << "\n── Part 63: SIMD Batch Filter Tests ──\n\n";

    // 63a: simdTypeLive16 — all live
    {
        uint8_t types[16];
        for (int i = 0; i < 16; i++) types[i] = 1;
        uint16_t mask = me::simdTypeLive16(types);
        CHECK(mask == 0xFFFF);
        std::cout << "  63a: typeLive16 all live         PASS\n";
    }

    // 63b: simdTypeLive16 — all dead (tombstones)
    {
        uint8_t types[16] = {};
        uint16_t mask = me::simdTypeLive16(types);
        CHECK(mask == 0);
        std::cout << "  63b: typeLive16 all dead         PASS\n";
    }

    // 63c: simdTypeLive16 — mixed
    {
        uint8_t types[16] = {};
        types[0] = 1;  // bit 0
        types[3] = 2;  // bit 3
        types[7] = 1;  // bit 7
        types[15] = 2; // bit 15
        uint16_t mask = me::simdTypeLive16(types);
        CHECK((mask & (1 << 0)) != 0);
        CHECK((mask & (1 << 3)) != 0);
        CHECK((mask & (1 << 7)) != 0);
        CHECK((mask & (1 << 15)) != 0);
        CHECK((mask & (1 << 1)) == 0);
        CHECK((mask & (1 << 4)) == 0);
        std::cout << "  63c: typeLive16 mixed            PASS\n";
    }

    // 63d: simdTypeEq16 — match type 1 (file)
    {
        uint8_t types[16] = {1, 2, 1, 0, 1, 2, 2, 1, 0, 0, 1, 1, 2, 1, 0, 1};
        uint16_t mask = me::simdTypeEq16(types, 1);
        // positions: 0,2,4,7,10,11,13,15
        CHECK((mask & (1 << 0)) != 0);
        CHECK((mask & (1 << 2)) != 0);
        CHECK((mask & (1 << 4)) != 0);
        CHECK((mask & (1 << 7)) != 0);
        CHECK((mask & (1 << 10)) != 0);
        CHECK((mask & (1 << 11)) != 0);
        CHECK((mask & (1 << 13)) != 0);
        CHECK((mask & (1 << 15)) != 0);
        // non-matching
        CHECK((mask & (1 << 1)) == 0);
        CHECK((mask & (1 << 3)) == 0);
        CHECK((mask & (1 << 5)) == 0);
        std::cout << "  63d: typeEq16 match type=1       PASS\n";
    }

    // 63e: simdTypeEq16 — match type 2 (folder)
    {
        uint8_t types[16] = {1, 2, 1, 0, 1, 2, 2, 1, 0, 0, 1, 1, 2, 1, 0, 1};
        uint16_t mask = me::simdTypeEq16(types, 2);
        // positions: 1,5,6,12
        CHECK((mask & (1 << 1)) != 0);
        CHECK((mask & (1 << 5)) != 0);
        CHECK((mask & (1 << 6)) != 0);
        CHECK((mask & (1 << 12)) != 0);
        CHECK((mask & (1 << 0)) == 0);
        std::cout << "  63e: typeEq16 match type=2       PASS\n";
    }

    // 63f: simdTypeEq16 — no matches
    {
        uint8_t types[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint16_t mask = me::simdTypeEq16(types, 2);
        CHECK(mask == 0);
        std::cout << "  63f: typeEq16 no matches         PASS\n";
    }

    // 63g: simdCompareU64x2GT
    {
        uint64_t vals[2] = {100, 50};
        CHECK(me::simdCompareU64x2GT(vals, 60) == 1);   // only vals[0]=100 > 60
        CHECK(me::simdCompareU64x2GT(vals, 40) == 3);   // both > 40
        CHECK(me::simdCompareU64x2GT(vals, 100) == 0);  // neither > 100
        CHECK(me::simdCompareU64x2GT(vals, 99) == 1);   // only vals[0]=100 > 99
        std::cout << "  63g: compareU64x2GT              PASS\n";
    }

    // 63h: simdCompareU64x2GE
    {
        uint64_t vals[2] = {100, 50};
        CHECK(me::simdCompareU64x2GE(vals, 100) == 1);  // vals[0]=100 >= 100
        CHECK(me::simdCompareU64x2GE(vals, 50) == 3);   // both >= 50
        CHECK(me::simdCompareU64x2GE(vals, 101) == 0);  // neither >= 101
        std::cout << "  63h: compareU64x2GE              PASS\n";
    }

    // 63i: simdCompareU64x2LT
    {
        uint64_t vals[2] = {100, 50};
        CHECK(me::simdCompareU64x2LT(vals, 60) == 2);   // only vals[1]=50 < 60
        CHECK(me::simdCompareU64x2LT(vals, 200) == 3);  // both < 200
        CHECK(me::simdCompareU64x2LT(vals, 50) == 0);   // neither < 50
        std::cout << "  63i: compareU64x2LT              PASS\n";
    }

    // 63j: simdCompareU64x2LE
    {
        uint64_t vals[2] = {100, 50};
        CHECK(me::simdCompareU64x2LE(vals, 100) == 3);  // both <= 100
        CHECK(me::simdCompareU64x2LE(vals, 50) == 2);   // only vals[1]=50 <= 50
        CHECK(me::simdCompareU64x2LE(vals, 49) == 0);   // neither <= 49
        std::cout << "  63j: compareU64x2LE              PASS\n";
    }

    // 63k: simdCompareU64x2EQ
    {
        uint64_t vals[2] = {100, 50};
        CHECK(me::simdCompareU64x2EQ(vals, 100) == 1);  // only vals[0]=100 == 100
        CHECK(me::simdCompareU64x2EQ(vals, 50) == 2);   // only vals[1]=50 == 50
        CHECK(me::simdCompareU64x2EQ(vals, 75) == 0);   // neither == 75
        std::cout << "  63k: compareU64x2EQ              PASS\n";
    }

    // 63l: simdCompareU64x2GT — edge case: both equal to threshold
    {
        uint64_t vals[2] = {42, 42};
        CHECK(me::simdCompareU64x2GT(vals, 42) == 0);
        CHECK(me::simdCompareU64x2GE(vals, 42) == 3);
        std::cout << "  63l: compareU64x2 edge equal     PASS\n";
    }

    // 63m: simdCompareU64x2 — edge case: zero values
    {
        uint64_t vals[2] = {0, 0};
        CHECK(me::simdCompareU64x2GT(vals, 0) == 0);
        CHECK(me::simdCompareU64x2GE(vals, 0) == 3);
        CHECK(me::simdCompareU64x2LT(vals, 1) == 3);
        CHECK(me::simdCompareU64x2EQ(vals, 0) == 3);
        std::cout << "  63m: compareU64x2 zeros          PASS\n";
    }

    // 63n: simdCompareU64x2 — large values
    {
        uint64_t vals[2] = {UINT64_MAX, UINT64_MAX - 1};
        CHECK(me::simdCompareU64x2GT(vals, UINT64_MAX - 1) == 1);  // only vals[0]
        CHECK(me::simdCompareU64x2EQ(vals, UINT64_MAX) == 1);
        CHECK(me::simdCompareU64x2EQ(vals, UINT64_MAX - 1) == 2);
        std::cout << "  63n: compareU64x2 large vals     PASS\n";
    }

    // 63o: simdTypeLive16 — single live record among dead
    {
        uint8_t types[16] = {};
        types[8] = 1;
        uint16_t mask = me::simdTypeLive16(types);
        CHECK(mask == (1 << 8));
        std::cout << "  63o: typeLive16 single live      PASS\n";
    }
}
