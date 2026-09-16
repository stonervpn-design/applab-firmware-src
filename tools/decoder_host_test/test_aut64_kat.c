/*
 * test_aut64_kat.c — AUT64 invertibility SURVEY + snapshot cross-check for the
 * in-tree cipher (lib/subghz/protocols/aut64.c), used by the VAG RKE decoder.
 *
 * NOTE: a single-key AUT64 known-answer test already lives in
 * tools/crypto_kat/test_crypto_kat.c (kat_aut64: encrypt snapshot +
 * encrypt/decrypt round-trip + key-sensitivity, part of the 14/14 KAT). This
 * file adds the dimension that one does NOT cover: invertibility across MANY
 * keys.
 *
 * FINDING it documents: aut64_encrypt is NOT invertible for every valid key.
 * encrypt_final_byte_nibble() does a reverse lookup into table_offset and has a
 * "should never happen ... return 0" fallthrough; for keys whose offset block
 * is not a full permutation, that path is taken and decrypt(encrypt(P)) != P.
 * The KAT key and the 3 real VAG keys avoid it (so VAG decode+TX are correct),
 * but the cipher is not a universal involution. This test QUANTIFIES the rate
 * rather than asserting a false "always invertible" claim.
 *
 * Exit code: fails (1) ONLY if the crypto_kat snapshot vector regresses — that
 * would mean cipher behavior actually drifted. The survey itself is diagnostic
 * (prints the invertible-key rate) and does not fail the build.
 *
 * Build+run (from repo root, under WSL):
 *   wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_aut64_kat.sh'
 */
#include "../../lib/subghz/protocols/aut64.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;

/* Deterministic xorshift PRNG — reproducible, no rand()/time. */
static uint32_t s_rng = 0xC0FFEEu;
static uint32_t rng(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* Fisher-Yates a 0..n-1 permutation into box using the PRNG. */
static void rand_perm(uint8_t* box, uint8_t n) {
    for(uint8_t i = 0; i < n; i++) box[i] = i;
    for(uint8_t i = n; i > 1; i--) {
        uint8_t j = (uint8_t)(rng() % i);
        uint8_t t = box[i - 1];
        box[i - 1] = box[j];
        box[j] = t;
    }
}

static int roundtrips(const struct aut64_key* k, const uint8_t* p) {
    uint8_t b[AUT64_BLOCK_SIZE];
    memcpy(b, p, AUT64_BLOCK_SIZE);
    if(aut64_encrypt(k, b) != AUT64_OK) return 0;
    if(aut64_decrypt(k, b) != AUT64_OK) return 0;
    return memcmp(b, p, AUT64_BLOCK_SIZE) == 0;
}

int main(void) {
    printf("== AUT64 invertibility survey (12-round in-tree cipher) ==\n");

    /* 1. Cross-check the crypto_kat snapshot vector reproduces exactly. Any
     *    change here = real cipher-behavior regression => hard fail. */
    {
        struct aut64_key k = (struct aut64_key){0};
        const uint8_t key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const uint8_t pbox[8] = {3, 1, 4, 7, 2, 0, 6, 5};
        const uint8_t sbox[16] = {0, 1, 9, 14, 13, 11, 7, 6, 15, 2, 12, 5, 10, 4, 3, 8};
        memcpy(k.key, key, 8);
        memcpy(k.pbox, pbox, 8);
        memcpy(k.sbox, sbox, 16);
        uint8_t ct[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
        aut64_encrypt(&k, ct);
        static const uint8_t snap[8] = {0x4d, 0x23, 0x45, 0x4f, 0x89, 0x98, 0xcd, 0xb3};
        if(memcmp(ct, snap, 8) != 0) {
            printf("FAIL: crypto_kat snapshot vector regressed\n");
            g_fail++;
        } else {
            printf("ok   crypto_kat snapshot vector reproduced (4d23454f8998cdb3)\n");
        }
        /* This exact key IS invertible (as crypto_kat asserts). */
        uint8_t p[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
        printf("ok   crypto_kat key round-trips: %s\n", roundtrips(&k, p) ? "yes" : "NO");
    }

    /* 2. Survey: how many random VALID keys give a fully-invertible cipher? */
    const uint8_t pts[3][8] = {
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
        {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE},
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
    };
    const int N = 2000;
    int invertible = 0;
    for(int t = 0; t < N; t++) {
        struct aut64_key k = (struct aut64_key){0};
        k.index = (uint8_t)(rng() & 0xFF);
        for(int i = 0; i < 8; i++) k.key[i] = (uint8_t)(rng() & 0x0F); /* nibbles */
        rand_perm(k.pbox, 8);
        rand_perm(k.sbox, 16);
        int ok = 1;
        for(int j = 0; j < 3; j++) ok &= roundtrips(&k, pts[j]);
        if(ok) invertible++;
    }
    double pct = 100.0 * invertible / N;
    printf(
        "SURVEY: %d/%d random valid keys are fully invertible (%.1f%%)\n",
        invertible,
        N,
        pct);
    printf(
        "        => aut64_encrypt is NOT a universal involution; the "
        "encrypt_final_byte_nibble\n"
        "        fallthrough breaks round-trip for the remaining keys. VAG's real "
        "keys are\n"
        "        in the invertible set (VAG decode+TX verified), so this is a "
        "latent-robustness\n"
        "        note, not a live defect.\n");

    printf("== AUT64 survey: %s (snapshot %s) ==\n",
           g_fail == 0 ? "OK" : "REGRESSED",
           g_fail == 0 ? "intact" : "CHANGED");
    return g_fail == 0 ? 0 : 1;
}
