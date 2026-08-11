#include "fuzzer_mutate.h"
#include <string.h>

static uint32_t fuzz_rng(uint32_t x) { // splitmix32
    x += 0x9E3779B9u; x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u; return x ^ (x >> 16);
}

static void seed_into(const FuzzConfig* c, FuzzFrame* o) {
    memset(o, 0, sizeof(*o));
    o->strategy = c->strategy; o->seed = c->seed;
    if(c->mode == FuzzModeProtocol) {
        o->nbits = c->seed_nbits;
        memcpy(o->bits, c->seed_bits, (c->seed_nbits + 7) / 8);
    } else {
        o->ndur = c->seed_ndur;
        memcpy(o->dur, c->seed_dur, c->seed_ndur * sizeof(int16_t));
    }
}

uint32_t fuzzer_mutate_count(const FuzzConfig* c) {
    switch(c->strategy) {
    case FuzzStrategyBitFlipSweep:
        return (c->mode == FuzzModeProtocol) ? c->seed_nbits : (uint32_t)c->seed_ndur * 16;
    case FuzzStrategyBoundary:
        return 4;
    case FuzzStrategyLength:
        return (uint32_t)(c->len_max_delta - c->len_min_delta + 1);
    case FuzzStrategyRandom:
        return c->rand_iters;
    case FuzzStrategyFieldWalk: {
        uint32_t n = (c->field_len >= 20) ? (1u << 20) : (1u << c->field_len);
        return n;
    }
    default:
        return 0;
    }
}

bool fuzzer_mutate_frame(const FuzzConfig* c, uint32_t index, FuzzFrame* o) {
    if(index >= fuzzer_mutate_count(c)) return false;
    seed_into(c, o);
    o->index = index;
    if(c->strategy == FuzzStrategyBitFlipSweep && c->mode == FuzzModeProtocol) {
        o->bits[index / 8] ^= (uint8_t)(0x80u >> (index % 8)); // bit 0 = MSB
        return true;
    }
    if(c->strategy == FuzzStrategyBoundary && c->mode == FuzzModeProtocol) {
        uint16_t nb = c->seed_nbits;
        switch(index) {
        case 0: memset(o->bits, 0x00, FUZZ_MAX_BYTES); break;
        case 1: memset(o->bits, 0xFF, FUZZ_MAX_BYTES); break;
        case 2: o->nbits = nb ? nb - 1 : 0; break;
        case 3: o->nbits = nb + 1; break;
        }
        return true;
    }
    if(c->strategy == FuzzStrategyLength && c->mode == FuzzModeProtocol) {
        o->nbits = (uint16_t)((int)c->seed_nbits + c->len_min_delta + (int)index);
        return true;
    }
    if(c->strategy == FuzzStrategyRandom && c->mode == FuzzModeProtocol) {
        uint32_t r = fuzz_rng(c->seed ^ index);
        uint16_t nbytes = (c->seed_nbits + 7) / 8;
        for(uint8_t i = 0; i < c->rand_k && nbytes; i++) {
            r = fuzz_rng(r);
            o->bits[r % nbytes] ^= (uint8_t)(r >> 8);
        }
        return true;
    }
    if(c->strategy == FuzzStrategyFieldWalk && c->mode == FuzzModeProtocol) {
        // write `index` into field_len bits at field_off (MSB-first)
        for(uint16_t i = 0; i < c->field_len; i++) {
            uint16_t bitpos = c->field_off + i;
            uint8_t bitval = (index >> (c->field_len - 1 - i)) & 1u;
            uint8_t* B = &o->bits[bitpos / 8];
            uint8_t m = (uint8_t)(0x80u >> (bitpos % 8));
            if(bitval) *B |= m; else *B &= (uint8_t)~m;
        }
        return true;
    }
    return true; // raw bit-flip handled in Task 2 extension
}
