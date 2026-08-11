#pragma once
#include <stdint.h>

/* PT2262 / PT2260 / SC2262 tri-state ADDRESS brute.
 *
 * A Princeton fixed code is 12 tri-state symbols, each pin = 0 / 1 / float — NOT
 * 24 arbitrary bits. So the address space is 3^N, not 2^24: sweeping the 8
 * address symbols is 3^8 = 6561 combinations (minutes), not days. Any PT2262 DIP-
 * switch doorbell rings the instant the swept address matches its DIP setting.
 *
 * Symbol -> 2 Princeton bits (MSB-first, matches the Flipper princeton encoder):
 *     0 -> 00b,  1 -> 11b,  float -> 01b (short-then-long).
 * Word layout (24-bit, MSB first): 8 address symbols = top 16 bits (A0 at
 * bits 23:22 .. A7 at 9:8); 4 data symbols = low 8 bits.
 *
 * VERIFIED on host (scratchpad/tristate2.py): db_tristate_word(2106, 0x55) ==
 * 0x005455 (a real intertechno DB code); the 6561 words are distinct, all address
 * pairs are valid tri-state, and every intertechno DB code is reachable. This is
 * the framed-Princeton .sub-replay waveform (PWM symbols + sync gap), the same
 * path a saved doorbell .sub uses to ring a bell — so a matching sweep rings it.
 */

#define DB_TRISTATE_COUNT 6561u /* 3^8 */

/* Base-3 address index n in [0,6561) + fixed 8-bit data -> 24-bit Princeton word. */
static inline uint32_t db_tristate_word(uint32_t n, uint8_t data) {
    static const uint8_t sym2bits[3] = {0x0u, 0x3u, 0x1u}; /* 0->00, 1->11, float->01 */
    uint32_t w = 0;
    uint32_t x = n;
    for(uint8_t p = 0; p < 8; p++) { /* p=0 -> A0 (bits 23:22) ... p=7 -> A7 (bits 9:8) */
        uint8_t d = (uint8_t)(x % 3u);
        x /= 3u;
        w |= (uint32_t)sym2bits[d] << (22u - 2u * p);
    }
    return w | (uint32_t)data;
}

/* Data symbols to sweep. 0x55 (all-float) is what 17/19 clean DB bells send and
 * rings the dominant float-code family; 0xFF (all-1) / 0x00 (all-0) catch the
 * other receiver wirings. */
static const uint8_t kDbDataSet[] = {0x55u, 0xFFu, 0x00u};
#define DB_DATA_COUNT ((uint8_t)(sizeof(kDbDataSet) / sizeof(kDbDataSet[0])))

/* Short-pulse (te) values to sweep, microseconds. The PT2272 RX self-clocks te
 * from the waveform but has an oscillator window; the doorbell DB spans ~180-420,
 * so two values (near the dominant intertechno ~355 first, then a lower one)
 * cover the common range. */
static const uint16_t kDbTeSet[] = {400u, 250u};
#define DB_TE_COUNT ((uint8_t)(sizeof(kDbTeSet) / sizeof(kDbTeSet[0])))
