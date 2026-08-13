#include "wlan_mgmt_fuzz.h"

#ifndef PUBLIC_BUILD

#include <string.h>

/* splitmix32 -- small, fast, deterministic PRNG step. Mirrors the mixing
 * idiom in applications/main/nrf24/helpers/nrf24_fuzz.c's fuzz_rng32(),
 * adapted to a stateful pointer form so a single mode's mutation logic can
 * chain multiple draws off one running state variable. NOT a CSPRNG, just
 * seed-reproducible scatter. */
static uint32_t sm32(uint32_t* s) {
    *s += 0x9E3779B9u;
    uint32_t z = *s;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

bool wlan_mgmt_fuzz_next(
    const uint8_t* base,
    uint16_t base_len,
    WlanFuzzMode mode,
    uint32_t seed,
    uint32_t step,
    uint8_t* out,
    uint16_t* out_len) {
    if(!base || !out || !out_len || base_len < 24) return false;

    uint16_t bl = base_len > WLAN_FUZZ_MAX ? (uint16_t)WLAN_FUZZ_MAX : base_len;
    memcpy(out, base, bl);

    uint32_t st = seed ^ (step * 0x9E3779B9u);

    switch(mode) {
    case WMF_BITFLIP: {
        *out_len = bl;
        uint32_t total_bits = (uint32_t)bl * 8u;
        uint32_t bit = sm32(&st) % total_bits;
        uint16_t byte_i = (uint16_t)(bit / 8u);
        uint8_t bit_i = (uint8_t)(bit % 8u);
        out[byte_i] ^= (uint8_t)(1u << bit_i);
        break;
    }
    case WMF_FIELD: {
        *out_len = bl;
        uint32_t field = sm32(&st) % 4u;
        uint32_t v = sm32(&st);
        switch(field) {
        case 0: /* frame-control: OR a random subtype into the upper nibble; stays mgmt-type */
            out[0] = (uint8_t)(out[0] | (v & 0xF0u));
            break;
        case 1: /* duration */
            out[2] = (uint8_t)v;
            out[3] = (uint8_t)(v >> 8);
            break;
        case 2: /* seqctl */
            out[22] = (uint8_t)v;
            out[23] = (uint8_t)(v >> 8);
            break;
        default: /* first body byte: reason/status/auth-alg, only if present */
            if(bl > 24) out[24] = (uint8_t)v;
            break;
        }
        break;
    }
    case WMF_IE_MANGLE: {
        *out_len = bl;
        if(bl > 24) {
            uint16_t o = (uint16_t)(24u + (sm32(&st) % (uint32_t)(bl - 24)));
            out[o] = (uint8_t)sm32(&st);
        }
        break;
    }
    case WMF_LENSWEEP: {
        uint16_t L = (uint16_t)(24u + (step % (uint32_t)(WLAN_FUZZ_MAX - 24 + 1)));
        for(uint16_t i = bl; i < L; i++) out[i] = (uint8_t)sm32(&st);
        *out_len = L;
        break;
    }
    case WMF_RANDOM:
    default: {
        /* header (out[0..23]) already carried over by the memcpy above. */
        uint16_t L = (uint16_t)(24u + (sm32(&st) % (uint32_t)(WLAN_FUZZ_MAX - 24 + 1)));
        for(uint16_t i = 24; i < L; i++) out[i] = (uint8_t)sm32(&st);
        *out_len = L;
        break;
    }
    }

    return true;
}

#endif /* PUBLIC_BUILD */
