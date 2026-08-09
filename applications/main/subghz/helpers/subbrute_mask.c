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

#endif // PUBLIC_BUILD
