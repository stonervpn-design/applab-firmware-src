// tools/crypto_kat/test_crypto_kat.c
//
// Known-answer + invariant regression tests for the firmware crypto primitives.
// Compiled against the REAL, unmodified lib/subghz/protocols/*.c sources (see
// Makefile). Purpose: pin correct behavior so a future refactor -- or a repeat
// of the historical Xtensa shift-UB that silently corrupted ~25% of KeeLoq keys
// -- fails loudly here instead of shipping. Returns non-zero on ANY failure so
// it can gate CI / a pre-flash check.
//
// Vector provenance:
//   KeeLoq  : github.com/hadipourh/KeeLoq README vectors (full 528-round),
//             independently reproduced; normal_learning golden derived from the
//             validated decrypt primitive.
//   AES-128 : NIST FIPS-197 Appendix C.1.
//   Hitag2  : canonical MIKRON vector (I.C. Wiener / Proxmark3), via the
//             cipher's own hitag2_selftest() plus an explicit ks32 check.
//   AUT64   : no config-matched public vector exists for this key layout, so we
//             pin an invertibility invariant + key-sensitivity + a behavior
//             snapshot (change detector).

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "keeloq_common.h"
#include "aes_common.h"
#include "hitag2_cipher.h"
#include "aut64.h"

static int g_checks = 0, g_fails = 0;

static void check(int ok, const char* name) {
    g_checks++;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if(!ok) g_fails++;
}

// ---------- KeeLoq (anchored: hadipourh/KeeLoq published 528-round vectors) ----------
static void kat_keeloq(void) {
    printf("KeeLoq  (lib/subghz/protocols/keeloq_common.c):\n");
    const uint64_t K = 0x5CEC6701B79FD949ULL;
    check(subghz_protocol_keeloq_common_encrypt(0xF741E2DBu, K) == 0xE44F4CDFu, "encrypt vector #1");
    check(subghz_protocol_keeloq_common_encrypt(0x0CA69B92u, K) == 0xA6AC0EA2u, "encrypt vector #2");
    check(subghz_protocol_keeloq_common_decrypt(0xE44F4CDFu, K) == 0xF741E2DBu, "decrypt vector #1");
    check(subghz_protocol_keeloq_common_decrypt(0xA6AC0EA2u, K) == 0x0CA69B92u, "decrypt vector #2");

    int rt_ok = 1;
    for(uint32_t x = 0; x < 0x100000u; x += 0x7A11u) {
        if(subghz_protocol_keeloq_common_decrypt(
               subghz_protocol_keeloq_common_encrypt(x, K), K) != x) {
            rt_ok = 0;
            break;
        }
    }
    check(rt_ok, "encrypt/decrypt round-trip (many blocks)");

    // Normal Learning (type 2) golden, derived from the validated decrypt primitive.
    check(
        subghz_protocol_keeloq_common_normal_learning(0x0A628B41u, K) == 0x112A0CFF0DB11650ULL,
        "normal_learning golden");
}

// ---------- AES-128 (anchored: NIST FIPS-197 Appendix C.1) ----------
static void kat_aes(void) {
    printf("AES-128 (lib/subghz/protocols/aes_common.c):\n");
    uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    uint8_t pt[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t ct_exp[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};

    uint8_t rk[176];
    aes_key_expansion(key, rk);

    uint8_t blk[16];
    memcpy(blk, pt, 16);
    aes128_encrypt(rk, blk);
    check(memcmp(blk, ct_exp, 16) == 0, "FIPS-197 encrypt vector");

    aes128_decrypt(rk, blk);
    check(memcmp(blk, pt, 16) == 0, "FIPS-197 decrypt round-trip");
}

// ---------- Hitag2 (anchored: MIKRON vector) ----------
static void kat_hitag2(void) {
    printf("Hitag2  (lib/subghz/protocols/hitag2_cipher.c):\n");
    check(hitag2_selftest(), "MIKRON self-test");

    uint64_t s = hitag2_init(0x4AD292B272F2ULL, 0x96EAC292u, 0x4EA276A6u);
    check(hitag2_ks32(&s) == 0xD7237FCEu, "MIKRON ks32");

    // RKE forge <-> counter-recovery self-consistency (the known-key forge path).
    const uint64_t key_onair = 0x1234567890ABULL;
    const uint32_t uid_onair = 0xDEADBEEFu;
    const uint32_t counter = 0x0123456u; // 28-bit rolling counter
    const uint8_t button = 0x2;
    uint32_t ks = hitag2_rke_ks(key_onair, uid_onair, counter, button);
    uint32_t rec = 0;
    int ok = hitag2_recover_counter(
        key_onair, uid_onair, (uint16_t)(counter & 0x3FFu), button, ks, &rec);
    check(ok && rec == counter, "RKE forge <-> counter-recovery round-trip");
}

// ---------- AUT64 (invertibility + key-sensitivity + behavior snapshot) ----------
static void kat_aut64(void) {
    printf("AUT64   (lib/subghz/protocols/aut64.c):\n");
    struct aut64_key k = (struct aut64_key){0};
    k.index = 0;
    const uint8_t key[8] = {1, 2, 3, 4, 5, 6, 7, 8};              // nibbles 0..15
    const uint8_t pbox[8] = {3, 1, 4, 7, 2, 0, 6, 5};            // perm of 0..7
    const uint8_t sbox[16] = {0, 1, 9, 14, 13, 11, 7, 6, 15, 2, 12, 5, 10, 4, 3, 8}; // perm of 0..15
    memcpy(k.key, key, 8);
    memcpy(k.pbox, pbox, 8);
    memcpy(k.sbox, sbox, 16);

    const uint8_t block[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

    uint8_t ct[8];
    memcpy(ct, block, 8);
    aut64_encrypt(&k, ct);
    printf(
        "    aut64 ct = %02x%02x%02x%02x%02x%02x%02x%02x\n",
        ct[0], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], ct[7]);
    // Behavior snapshot (regression pin) -- captured from the first clean run.
    const uint8_t snap[8] = {0x4d, 0x23, 0x45, 0x4f, 0x89, 0x98, 0xcd, 0xb3};
    check(memcmp(ct, snap, 8) == 0, "encrypt behavior snapshot");

    uint8_t rt[8];
    memcpy(rt, ct, 8);
    aut64_decrypt(&k, rt);
    check(memcmp(rt, block, 8) == 0, "encrypt/decrypt round-trip (invertibility)");

    struct aut64_key k2 = k;
    k2.key[0] ^= 0x1u;
    uint8_t ct2[8];
    memcpy(ct2, block, 8);
    aut64_encrypt(&k2, ct2);
    check(memcmp(ct, ct2, 8) != 0, "key sensitivity (1-nibble change diffuses)");
}

int main(void) {
    printf("=== Firmware crypto known-answer tests ===\n\n");
    kat_keeloq();
    kat_aes();
    kat_hitag2();
    kat_aut64();
    printf("\n%d/%d checks passed", g_checks - g_fails, g_checks);
    if(g_fails) {
        printf("  --  %d FAILED\n", g_fails);
        return 1;
    }
    printf("  --  ALL PASS\n");
    return 0;
}
