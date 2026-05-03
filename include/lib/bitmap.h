// SPDX-License-Identifier: Apache-2.0
// include/lib/bitmap.h
//
/// @brief Generic fixed-width bitmap over an array of uint64_t words.
///
///        Bit ordering: within each word, bit 0 of the logical bitmap maps
///        to the MSB of the word (bit 63 of the uint64_t).  This matches
///        the s390x convention used by hardware facility lists (STFLE) and
///        makes visual inspection of memory dumps natural on big-endian
///        systems.
///
///        All functions are static inline — zero call overhead, no .c file
///        needed.  The caller declares storage as:
///
///            uint64_t my_bitmap[BITMAP_WORDS(n)];
///
///        and passes it with the logical bit count n to each function.

#pragma once

#include <zxfoundation/types.h>
#include <lib/string.h>

/// @brief Number of uint64_t words needed to hold n bits.
#define BITMAP_WORDS(n)     (((n) + 63) / 64)

/// @brief Size in bytes of a bitmap holding n bits.
#define BITMAP_BYTES(n)     (BITMAP_WORDS(n) * sizeof(uint64_t))

// ---------------------------------------------------------------------------
// Single-bit operations
// ---------------------------------------------------------------------------

/// @brief Set bit i (mark as 1).
static inline void bitmap_set(uint64_t *bm, uint64_t i) {
    bm[i / 64] |= (UINT64_C(1) << (63 - (i % 64)));
}

/// @brief Clear bit i (mark as 0).
static inline void bitmap_clear(uint64_t *bm, uint64_t i) {
    bm[i / 64] &= ~(UINT64_C(1) << (63 - (i % 64)));
}

/// @brief Test bit i.
/// @return true if set.
static inline bool bitmap_test(const uint64_t *bm, uint64_t i) {
    return (bm[i / 64] >> (63 - (i % 64))) & UINT64_C(1);
}

/// @brief Atomically set bit i and return its previous value.
static inline bool bitmap_test_and_set(uint64_t *bm, uint64_t i) {
    uint64_t mask = UINT64_C(1) << (63 - (i % 64));
    uint64_t old  = bm[i / 64];
    bm[i / 64]   |= mask;
    return (old & mask) != 0;
}

/// @brief Atomically clear bit i and return its previous value.
static inline bool bitmap_test_and_clear(uint64_t *bm, uint64_t i) {
    uint64_t mask = UINT64_C(1) << (63 - (i % 64));
    uint64_t old  = bm[i / 64];
    bm[i / 64]   &= ~mask;
    return (old & mask) != 0;
}

// ---------------------------------------------------------------------------
// Range operations
// ---------------------------------------------------------------------------

/// @brief Set all bits in [start, start+len).
static inline void bitmap_set_range(uint64_t *bm, uint64_t start, uint64_t len) {
    if (!len) return;
    uint64_t end = start + len;
    uint64_t w0 = start / 64, b0 = start % 64;
    uint64_t w1 = (end - 1) / 64, b1 = (end - 1) % 64;
    if (w0 == w1) {
        // Same word: set bits b0..b1 (MSB-first ordering).
        for (uint64_t b = b0; b <= b1; b++)
            bm[w0] |= UINT64_C(1) << (63 - b);
        return;
    }
    // First partial word.
    for (uint64_t b = b0; b < 64; b++)
        bm[w0] |= UINT64_C(1) << (63 - b);
    // Full words.
    for (uint64_t w = w0 + 1; w < w1; w++)
        bm[w] = UINT64_MAX;
    // Last partial word.
    for (uint64_t b = 0; b <= b1; b++)
        bm[w1] |= UINT64_C(1) << (63 - b);
}

/// @brief Clear all bits in [start, start+len).
static inline void bitmap_clear_range(uint64_t *bm, uint64_t start, uint64_t len) {
    if (!len) return;
    uint64_t end = start + len;
    uint64_t w0 = start / 64, b0 = start % 64;
    uint64_t w1 = (end - 1) / 64, b1 = (end - 1) % 64;
    if (w0 == w1) {
        for (uint64_t b = b0; b <= b1; b++)
            bm[w0] &= ~(UINT64_C(1) << (63 - b));
        return;
    }
    for (uint64_t b = b0; b < 64; b++)
        bm[w0] &= ~(UINT64_C(1) << (63 - b));
    for (uint64_t w = w0 + 1; w < w1; w++)
        bm[w] = 0;
    for (uint64_t b = 0; b <= b1; b++)
        bm[w1] &= ~(UINT64_C(1) << (63 - b));
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

/// @brief Find the first set bit at or after position start.
/// @param nbits  Total number of valid bits in the bitmap.
/// @return       Bit index, or nbits if none found.
static inline uint64_t bitmap_find_next_set(const uint64_t *bm,
                                             uint64_t nbits,
                                             uint64_t start) {
    for (uint64_t i = start; i < nbits; i++)
        if (bitmap_test(bm, i))
            return i;
    return nbits;
}

/// @brief Find the first clear bit at or after position start.
/// @return Bit index, or nbits if none found.
static inline uint64_t bitmap_find_next_clear(const uint64_t *bm,
                                               uint64_t nbits,
                                               uint64_t start) {
    for (uint64_t i = start; i < nbits; i++)
        if (!bitmap_test(bm, i))
            return i;
    return nbits;
}

/// @brief Find the first run of n consecutive clear bits at or after start.
/// @return Starting bit index of the run, or nbits if not found.
static inline uint64_t bitmap_find_next_clear_run(const uint64_t *bm,
                                                   uint64_t nbits,
                                                   uint64_t start,
                                                   uint64_t n) {
    uint64_t run_start = start, run_len = 0;
    for (uint64_t i = start; i < nbits; i++) {
        if (!bitmap_test(bm, i)) {
            if (run_len == 0)
                run_start = i;
            if (++run_len == n)
                return run_start;
        } else {
            run_len = 0;
        }
    }
    return nbits;
}

/// @brief Find the first run of n consecutive set bits at or after start.
/// @return Starting bit index of the run, or nbits if not found.
static inline uint64_t bitmap_find_next_set_run(const uint64_t *bm,
                                                 uint64_t nbits,
                                                 uint64_t start,
                                                 uint64_t n) {
    uint64_t run_start = start, run_len = 0;
    uint64_t i = start;
    while (i < nbits) {
        uint64_t word_idx = i / 64;
        uint64_t bit_off  = i % 64;
        uint64_t word     = bm[word_idx];

        if (!((word >> (63 - bit_off)) & 1)) {
            run_len = 0;
            // Skip clear bits in this word using clzll on the remaining bits.
            // Shift so bit_off maps to MSB, then find first set bit.
            uint64_t mask = (bit_off < 63) ? (word << bit_off) : 0;
            if (!mask) {
                i = (word_idx + 1) * 64;
                continue;
            }
            uint64_t skip = (uint64_t)__builtin_clzll(mask);
            i += skip;
            run_start = i;
            continue;
        }

        // Current bit is set.
        if (bit_off == 0 && word == UINT64_MAX && run_len + 64 <= n) {
            if (run_len == 0) run_start = i;
            run_len += 64;
            i       += 64;
            if (run_len >= n) return run_start;
            continue;
        }

        if (run_len == 0) run_start = i;
        if (++run_len >= n) return run_start;
        i++;
    }
    return nbits;
}

// ---------------------------------------------------------------------------
// Whole-bitmap utilities
// ---------------------------------------------------------------------------

/// @brief Zero all bits in a bitmap of nwords words.
static inline void bitmap_zero(uint64_t *bm, uint64_t nwords) {
    memset(bm, 0, nwords * sizeof(uint64_t));
}

/// @brief Count the number of set bits (popcount) across nwords words.
static inline uint64_t bitmap_popcount(const uint64_t *bm, uint64_t nwords) {
    uint64_t count = 0;
    for (uint64_t w = 0; w < nwords; w++)
        count += (uint64_t)__builtin_popcountll(bm[w]);
    return count;
}
