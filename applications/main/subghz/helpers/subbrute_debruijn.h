#pragma once

/**
 * De Bruijn sequence B(2, n) generator for Sub-GHz fixed-code brute force.
 *
 * A de Bruijn sequence contains every n-bit combination exactly once in a
 * single sliding window, so one continuous OOK burst covers all 2^n codes
 * instead of 2^n separately-framed transmissions. Ported (clean reimpl of the
 * greedy "prefer-ones" construction) from EvilCrowRF-V2 (MIT), Senape3000.
 *
 * The algorithm core is furi-independent so it can be host-tested.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hard cap: n<=16 => 64KiB sequence + 8KiB visited bitmap. */
#define SUBBRUTE_DEBRUIJN_MAX_N 16
#define SUBBRUTE_DEBRUIJN_MIN_N 1

/**
 * Generate a de Bruijn B(2,n) sequence as a linear array of 0/1 bytes.
 * Every n-bit window appears exactly once; length = 2^n + n - 1.
 *
 * @param n         bits per code (SUBBRUTE_DEBRUIJN_MIN_N..SUBBRUTE_DEBRUIJN_MAX_N)
 * @param out_len   receives the sequence length in bytes (one byte per bit)
 * @return          malloc'd 0/1 byte array (caller frees), or NULL on bad n / OOM
 */
uint8_t* subbrute_debruijn_generate(uint8_t n, size_t* out_len);

/** Sequence length that generate() will produce for a given n (0 if n invalid). */
size_t subbrute_debruijn_length(uint8_t n);

/**
 * Practical cap for RAW transmission (keeps the RAW_Data string bounded).
 * n=12 => 4107 bits; higher n balloons the duration string.
 */
#define SUBBRUTE_DEBRUIJN_MAX_TX_N 12

/** Practical lower bound for the attack-menu n selector (n=8 => 255 codes). */
#define SUBBRUTE_DEBRUIJN_MIN_TX_N 8

/**
 * Render a de Bruijn bit sequence into a Flipper RAW_Data duration string
 * (space-separated signed microseconds), OOK: a run of k ones => +(k*te),
 * a run of k zeros => -(k*te). The sequence must start with a 1 (generate()
 * always does), so the string begins with a positive (carrier-on) duration.
 *
 * @param seq   0/1 byte sequence from subbrute_debruijn_generate()
 * @param len   sequence length
 * @param te    per-bit duration in microseconds (e.g. 250, 350)
 * @return      malloc'd NUL-terminated string (caller frees), or NULL on error
 */
char* subbrute_debruijn_raw_data(const uint8_t* seq, size_t len, uint32_t te);

#ifdef __cplusplus
}
#endif
