#include "hitag2_cipher.h"

// Pick 4 state bits into a 4-bit index ('a' = LSB).
#define HT2_I4(x, a, b, c, d)                                              \
    ((uint32_t)((((x) >> (a)) & 1) | ((((x) >> (b)) & 1) << 1) |           \
                ((((x) >> (c)) & 1) << 2) | ((((x) >> (d)) & 1) << 3)))

// Nonlinear filter tables (bit-packed: table(i) = (CONST >> i) & 1).
static const uint32_t HT2_F4A = 0x2C79, HT2_F4B = 0x6671, HT2_F5C = 0x7907287B;

// f20: 20 selected state bits -> 1 keystream bit.
static uint32_t hitag2_f20(uint64_t x) {
    uint32_t i5 = ((HT2_F4A >> HT2_I4(x, 1, 2, 4, 5)) & 1) * 1 +
                  ((HT2_F4B >> HT2_I4(x, 7, 11, 13, 14)) & 1) * 2 +
                  ((HT2_F4B >> HT2_I4(x, 16, 20, 22, 25)) & 1) * 4 +
                  ((HT2_F4B >> HT2_I4(x, 27, 28, 30, 32)) & 1) * 8 +
                  ((HT2_F4A >> HT2_I4(x, 33, 42, 43, 45)) & 1) * 16;
    return (HT2_F5C >> i5) & 1;
}

// One keystream round: shift RIGHT (feedback bit in at bit 47), then f20 of the
// NEW state. Feedback taps {0,2,3,6,7,8,16,22,23,26,30,41,42,43,46,47}.
static uint32_t hitag2_round(uint64_t* s) {
    uint64_t x = *s;
    uint32_t fb = (uint32_t)((x >> 0) ^ (x >> 2) ^ (x >> 3) ^ (x >> 6) ^ (x >> 7) ^
                             (x >> 8) ^ (x >> 16) ^ (x >> 22) ^ (x >> 23) ^ (x >> 26) ^
                             (x >> 30) ^ (x >> 41) ^ (x >> 42) ^ (x >> 43) ^ (x >> 46) ^
                             (x >> 47)) &
                  1;
    x = (x >> 1) | ((uint64_t)fb << 47);
    *s = x;
    return hitag2_f20(x);
}

uint64_t hitag2_init(uint64_t key, uint32_t serial, uint32_t iv) {
    // serial in bits 0..31, low 16 key bits in bits 32..47.
    uint64_t x = ((key & 0xFFFF) << 32) | serial;
    // Exactly 32 warm-up rounds; each inserts f20 ^ iv_bit_i ^ key_bit_(i+16).
    // The LFSR feedback polynomial is NOT applied during init.
    for(uint32_t i = 0; i < 32; i++) {
        x >>= 1;
        uint32_t b = hitag2_f20(x) ^ (((iv >> i) ^ (uint32_t)(key >> (i + 16))) & 1);
        x |= (uint64_t)b << 47;
    }
    return x;
}

uint8_t hitag2_byte(uint64_t* s) {
    uint8_t c = 0;
    for(int i = 0; i < 8; i++) c |= (uint8_t)(hitag2_round(s) << (i ^ 7)); // MSB-first
    return c;
}

uint32_t hitag2_ks32(uint64_t* s) {
    uint32_t v = 0;
    for(int i = 0; i < 4; i++) v = (v << 8) | hitag2_byte(s); // big-endian
    return v;
}

uint32_t hitag2_rev32(uint32_t v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}

uint64_t hitag2_rev48(uint64_t v) {
    uint64_t r = 0;
    for(int i = 0; i < 48; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

bool hitag2_selftest(void) {
    // MIKRON authoritative vector (I.C. Wiener / Proxmark3 / RFIDler).
    static const uint8_t expected[16] = {
        0xD7, 0x23, 0x7F, 0xCE, 0x8C, 0xD0, 0x37, 0xA9,
        0x57, 0x49, 0xC1, 0xE6, 0x48, 0x00, 0x8A, 0xB6};
    uint64_t s = hitag2_init(0x4AD292B272F2ull, 0x96EAC292u, 0x4EA276A6u);
    for(int i = 0; i < 16; i++)
        if(hitag2_byte(&s) != expected[i]) return false;
    return true;
}
