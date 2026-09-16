// Host verification of the ported Hitag2Hell recovery (helpers/subghz_hitag2_*).
//   1) hitag2_fiat_self_test(): cross-checks the ported u64-packed core cipher
//      against an INDEPENDENT copy of fiat_v1.c's byte-array cipher (below) AND
//      that init-phase inversion recovers the key.
//   2) hitag2_hell_self_test(): the bitsliced Hell kernel recovers the true
//      state31 for all 8 known Fiat V1 keys (pointed at the correct L0 slot).
#include <stdio.h>
#include <stdint.h>

#include "subghz_hitag2_core.h"
#include "subghz_hitag2_hell.h"

// --- Independent reference: faithful copy of fiat_v1.c's byte-array cipher ---
static uint8_t r_truth(uint32_t t, uint8_t i) { return (uint8_t)((t >> i) & 1U); }
static uint8_t r_fi(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint8_t)((a << 3) | (b << 2) | (c << 1) | d);
}
static uint8_t r_bb(uint8_t byte, uint8_t bit) { return (uint8_t)((byte >> bit) & 1U); }
static uint8_t r_filter(const uint8_t s[6]) {
    uint8_t g = 0;
    g |= r_truth(0x2c79U, r_fi(r_bb(s[0], 1), r_bb(s[0], 2), r_bb(s[0], 4), r_bb(s[0], 5)));
    g |= (uint8_t)(r_truth(0x6671U, r_fi(r_bb(s[1], 0), r_bb(s[1], 1), r_bb(s[1], 3), r_bb(s[1], 7))) << 1);
    g |= (uint8_t)(r_truth(0x6671U, r_fi(r_bb(s[3], 5), r_bb(s[2], 0), r_bb(s[2], 2), r_bb(s[2], 6))) << 2);
    g |= (uint8_t)(r_truth(0x6671U, r_fi(r_bb(s[4], 6), r_bb(s[3], 0), r_bb(s[3], 2), r_bb(s[3], 3))) << 3);
    g |= (uint8_t)(r_truth(0x2c79U, r_fi(r_bb(s[5], 1), r_bb(s[5], 3), r_bb(s[5], 4), r_bb(s[4], 5))) << 4);
    return r_truth(0x7907287bUL, g);
}
static uint8_t r_par(uint8_t v) {
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return v & 1U;
}
static uint8_t r_feedback(const uint8_t s[6]) {
    static const uint8_t m[6] = {0xb3U, 0x80U, 0x83U, 0x22U, 0x00U, 0x73U};
    uint8_t f = 0;
    for(int i = 0; i < 6; i++) f ^= r_par((uint8_t)(s[i] & m[i]));
    return f & 1U;
}
static void r_shift(uint8_t s[6], uint8_t in) {
    for(int i = 0; i < 5; i++) s[i] = (uint8_t)((s[i] << 1) | (s[i + 1] >> 7));
    s[5] = (uint8_t)((s[5] << 1) | (in & 1U));
}
static uint8_t r_ibu(uint32_t v, uint8_t i) { return (uint8_t)((v >> (31 - i)) & 1U); }
static uint8_t r_ibb(const uint8_t* b, uint8_t i) { return (uint8_t)((b[i >> 3] >> (7 - (i & 7))) & 1U); }

uint32_t subghz_protocol_fiat_v1_compute_auth(
    uint32_t uid, uint8_t button, uint16_t control, const uint8_t key[6], uint32_t epoch) {
    uint8_t s[6] = {(uint8_t)(uid >> 24), (uint8_t)(uid >> 16), (uint8_t)(uid >> 8),
                    (uint8_t)uid, key[4], key[5]};
    uint32_t iv = ((epoch & 0x3ffffUL) << 14) | (((uint32_t)control & 0x3ffUL) << 4) |
                  ((uint32_t)button & 0xfUL);
    for(int i = 0; i < 32; i++) {
        uint8_t in = (uint8_t)(r_ibu(iv, (uint8_t)i) ^ r_ibb(key, (uint8_t)i) ^ r_filter(s));
        r_shift(s, in);
    }
    uint32_t a = 0;
    for(int i = 0; i < 32; i++) {
        a = (a << 1) | r_filter(s);
        r_shift(s, r_feedback(s));
    }
    return a;
}

int main(void) {
    printf("[1/2] core u64 cipher == fiat_v1 byte-array + invert round-trip ... ");
    fflush(stdout);
    int core_ok = hitag2_fiat_self_test() ? 1 : 0;
    printf("%s\n", core_ok ? "PASS" : "FAIL");

    printf("[2/2] Hell kernel recovers state31 for all 8 keys (~5-15s) ... ");
    fflush(stdout);
    int hell_ok = hitag2_hell_self_test() ? 1 : 0;
    printf("%s\n", hell_ok ? "PASS" : "FAIL");

    int ok = core_ok && hell_ok;
    printf("\nRESULT: %s\n", ok ? "ALL PASS (Hitag2Hell port verified)" : "FAIL");
    return ok ? 0 : 1;
}
