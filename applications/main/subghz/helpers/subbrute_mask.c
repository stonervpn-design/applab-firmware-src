#include "subbrute_mask.h"

#ifndef PUBLIC_BUILD

uint8_t subbrute_mask_var_bits(uint64_t var_mask) {
    uint8_t n = 0;
    while(var_mask) {
        var_mask &= var_mask - 1; // clear the lowest set bit
        n++;
    }
    return n;
}

uint64_t subbrute_mask_max_value(uint64_t var_mask) {
    uint8_t k = subbrute_mask_var_bits(var_mask);
    if(k == 0 || k > 63) return 0; // degenerate, or would overflow uint64_t
    return (uint64_t)1 << k;
}

uint64_t subbrute_mask_candidate(uint64_t fixed, uint64_t var_mask, uint64_t step) {
    // Start from the fixed bits with the masked positions cleared, then deposit
    // step's low bits into the masked positions (parallel bit deposit / PDEP).
    uint64_t result = fixed & ~var_mask;
    uint64_t m = var_mask;
    while(m) {
        uint64_t low = m & (~m + 1); // lowest set bit of the remaining mask (m & -m)
        if(step & 1u) result |= low;
        step >>= 1;
        m &= m - 1; // clear that bit and advance
    }
    return result;
}

uint64_t subbrute_mask_default_for_chip(SubGhzChipFamily family, int32_t bits) {
    uint8_t n;
    switch(family) {
    case SubGhzChipPrinceton: // PT2262/EV1527 etc: data nibble (EV1527 = 20-bit id + 4 data)
    case SubGhzChipCame:
    case SubGhzChipNice:
    case SubGhzChipHoltek: // HT12E: 8 address + 4 data
    case SubGhzChipAnsonic:
        n = 4;
        break;
    case SubGhzChipChamberlain: // DIP-switch remotes: button is a couple of bits
    case SubGhzChipLinear:
    case SubGhzChipLinearDelta:
        n = 2;
        break;
    default: // Unknown chip: conservative middle-ground nibble
        n = 4;
        break;
    }
    if(bits > 0 && n > (uint8_t)bits) n = (uint8_t)bits; // never wider than the frame
    if(n == 0) n = 1;
    return ((uint64_t)1 << n) - 1u; // low n bits variable, rest fixed from the capture
}

#endif // PUBLIC_BUILD
