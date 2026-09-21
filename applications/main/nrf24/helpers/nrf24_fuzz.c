#include "nrf24_fuzz.h"

/* splitmix32 -- small, fast, deterministic PRNG step. Mirrors
 * components/fuzz_core/fuzzer_mutate.c's fuzz_rng() so the two fuzzers
 * share a proven mixing function; NOT a CSPRNG, just seed-reproducible
 * scatter for N24F_RANDOM. */
static uint32_t fuzz_rng32(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

/* Clamp base_len into [1, 32]: 0 -> 1 (nothing to index, treat as a
 * single implicit zero byte), >32 -> 32 (truncate to the fixed out[32]
 * buffer). See header: "base_len/out_len 1..32". */
static uint8_t clamp_len(uint8_t len) {
    if(len == 0) return 1;
    if(len > 32) return 32;
    return len;
}

void nrf24_fuzz_next(
    const uint8_t* base,
    uint8_t base_len,
    Nrf24FuzzMode mode,
    uint32_t seed,
    uint32_t step,
    uint8_t out[32],
    uint8_t* out_len) {
    if(!out || !out_len) return; /* NULL out/out_len: safe no-op */

    uint8_t bl = clamp_len(base_len);

    /* Local copy of base, safely zero-padded/truncated to `bl` bytes.
     * Lets every mode below read `safe_base[i]` for i < bl without
     * re-checking `base` for NULL each time (NULL base is only valid
     * for N24F_RANDOM, which never reads safe_base). */
    uint8_t safe_base[32] = {0};
    if(base) {
        for(uint8_t i = 0; i < bl; i++) safe_base[i] = base[i];
    }

    switch(mode) {
    case N24F_BITFLIP: {
        for(uint8_t i = 0; i < bl; i++) out[i] = safe_base[i];
        *out_len = bl;
        uint32_t total_bits = (uint32_t)bl * 8u;
        uint32_t idx = step % total_bits;
        uint8_t byte_i = (uint8_t)(idx / 8);
        uint8_t bit_i = (uint8_t)(idx % 8);
        out[byte_i] ^= (uint8_t)(1u << bit_i);
        break;
    }
    case N24F_LENSWEEP: {
        uint8_t ol = (uint8_t)(1 + (step % 32));
        for(uint8_t i = 0; i < ol; i++) out[i] = safe_base[i % bl];
        *out_len = ol;
        break;
    }
    case N24F_FIELD: {
        for(uint8_t i = 0; i < bl; i++) out[i] = safe_base[i];
        *out_len = bl;
        uint8_t byte_idx = (uint8_t)((step / 256u) % bl);
        out[byte_idx] = (uint8_t)(step % 256u);
        break;
    }
    case N24F_RANDOM: {
        uint32_t s = seed ^ (step * 2654435761u);
        for(uint8_t i = 0; i < bl; i++) {
            s = fuzz_rng32(s);
            out[i] = (uint8_t)(s & 0xFFu);
        }
        *out_len = bl;
        break;
    }
    default:
        *out_len = 0;
        break;
    }
}
