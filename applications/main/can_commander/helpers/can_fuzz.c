#include "can_fuzz.h"
#include <string.h>

/* splitmix32 -- small, fast, deterministic PRNG step. Same constants as
 * components/fuzz_core/fuzzer_mutate.c's fuzz_rng() / nrf24_fuzz.c's
 * fuzz_rng32(), adapted to a state-pointer form so callers can thread one
 * running state (`st`) through multiple draws per mutation. NOT a CSPRNG,
 * just seed-reproducible scatter. */
static uint32_t sm32(uint32_t* s) {
    uint32_t z = (*s += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

void can_fuzz_next(const CanFrame* base, CanFuzzMode mode, uint32_t seed, uint32_t step, CanFrame* out) {
    if(!base || !out) return; /* NULL base/out: safe no-op */

    *out = *base; /* carries id/extended/rtr/dlc/data */

    uint32_t st = seed ^ (step * 0x9E3779B9u);

    switch(mode) {
    case CFZ_DATA_BITFLIP: {
        if(out->dlc > 0) {
            uint32_t bit = sm32(&st) % ((uint32_t)out->dlc * 8u);
            out->data[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        break;
    }
    case CFZ_DATA_RANDOM: {
        for(uint8_t i = 0; i < out->dlc; i++) out->data[i] = (uint8_t)sm32(&st);
        break;
    }
    case CFZ_ID_SWEEP: {
        uint32_t max = out->extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
        out->id = step % (max + 1u);
        break;
    }
    case CFZ_DLC_SWEEP: {
        out->dlc = (uint8_t)(step % 9u);
        for(uint8_t i = 0; i < out->dlc; i++) out->data[i] = (uint8_t)sm32(&st);
        break;
    }
    case CFZ_FULL_RANDOM:
    default: {
        uint32_t max = out->extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
        out->id = sm32(&st) & max;
        out->dlc = (uint8_t)(sm32(&st) % 9u);
        for(uint8_t i = 0; i < out->dlc; i++) out->data[i] = (uint8_t)sm32(&st);
        break;
    }
    }

    /* Safety clamp: ID_SWEEP is already <= max via modulo (harmless here);
     * this guards the random paths and any future mode against ever
     * emitting a frame can_hal_transmit would reject. */
    if(out->dlc > 8) out->dlc = 8;
    uint32_t m2 = out->extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
    out->id &= m2;
}
