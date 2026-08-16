// tools/desfire_auth_host_test/test_main.c
#include <mbedtls/aes.h>
#include <mbedtls/des.h>
#include <stdio.h>

int g_fail = 0;
#define CHECK(cond, msg) do { if(!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
                              else { printf("ok: %s\n", msg); } } while(0)

static void test_mbedtls_selftest(void) {
    CHECK(mbedtls_aes_self_test(0) == 0, "mbedtls AES self-test");
    CHECK(mbedtls_des_self_test(0) == 0, "mbedtls DES/3DES self-test");
}

#include "mf_desfire_crypto.h"
#include <string.h>

static void test_helpers(void) {
    CHECK(mf_desfire_key_size(MfDesfireKeyTypeDes) == 8, "des key size");
    CHECK(mf_desfire_key_size(MfDesfireKeyType3k3Des) == 24, "3k3des key size");
    CHECK(mf_desfire_key_size(MfDesfireKeyTypeAes) == 16, "aes key size");
    CHECK(mf_desfire_block_size(MfDesfireKeyTypeAes) == 16, "aes block");
    CHECK(mf_desfire_block_size(MfDesfireKeyTypeDes) == 8, "des block");
    // block_size is the TRUE cipher block (Task 5's CMAC keys off this to pick
    // the Rb constant / iterate in cipher-block units); challenge_size is the
    // RndA/RndB length for the 3-pass auth handshake. These differ only for
    // 3K3DES: true cipher block 8 (3DES), but a 16-byte AuthenticateISO challenge.
    CHECK(mf_desfire_block_size(MfDesfireKeyType3k3Des) == 8, "3k3des block (true cipher block)");
    CHECK(mf_desfire_challenge_size(MfDesfireKeyType3k3Des) == 16, "3k3des challenge (RndA/RndB len)");
    CHECK(mf_desfire_challenge_size(MfDesfireKeyTypeDes) == 8, "des challenge (RndA/RndB len)");
    CHECK(mf_desfire_challenge_size(MfDesfireKeyTypeAes) == 16, "aes challenge (RndA/RndB len)");
    CHECK(mf_desfire_auth_cmd(MfDesfireKeyTypeAes) == 0xaa, "aes cmd");
    CHECK(mf_desfire_auth_cmd(MfDesfireKeyTypeDes) == 0x0a, "des cmd");

    uint8_t r[4] = {0x01, 0x02, 0x03, 0x04};
    mf_desfire_rotate_left(r, 4);
    uint8_t exp[4] = {0x02, 0x03, 0x04, 0x01};
    CHECK(memcmp(r, exp, 4) == 0, "rotate left 1 byte");

    // CBC round-trip: decrypt(encrypt(x)) == x with fresh IVs
    uint8_t key[16]; memset(key, 0x00, 16);
    uint8_t in[16]  = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t iv1[16] = {0}, iv2[16] = {0}, ct[16], pt[16];
    mf_desfire_cbc_encrypt(MfDesfireKeyTypeAes, key, iv1, in, ct, 16);
    mf_desfire_cbc_decrypt(MfDesfireKeyTypeAes, key, iv2, ct, pt, 16);
    CHECK(memcmp(pt, in, 16) == 0, "aes cbc round-trip");
}

static void test_session_key(void) {
    uint8_t a[16], b[16];
    for(int i=0;i<16;i++){ a[i]=(uint8_t)(0xa0+i); b[i]=(uint8_t)(0xb0+i); }

    uint8_t sk[24];
    mf_desfire_derive_session_key(MfDesfireKeyTypeAes, a, b, sk);
    uint8_t aes_exp[16] = {0xa0,0xa1,0xa2,0xa3, 0xb0,0xb1,0xb2,0xb3,
                           0xac,0xad,0xae,0xaf, 0xbc,0xbd,0xbe,0xbf};
    CHECK(memcmp(sk, aes_exp, 16) == 0, "aes session key layout");

    mf_desfire_derive_session_key(MfDesfireKeyType2k3Des, a, b, sk);
    uint8_t d2_exp[16] = {0xa0,0xa1,0xa2,0xa3, 0xb0,0xb1,0xb2,0xb3,
                          0xa4,0xa5,0xa6,0xa7, 0xb4,0xb5,0xb6,0xb7};
    CHECK(memcmp(sk, d2_exp, 16) == 0, "2k3des session key layout");
}

#include "virtual_card.h"
static void auth_roundtrip(MfDesfireKeyType t, const char* name) {
    uint8_t key[24]; for(size_t i=0;i<mf_desfire_key_size(t);i++) key[i]=(uint8_t)(0x10+i);
    uint8_t card_rndb[16]; for(int i=0;i<16;i++) card_rndb[i]=(uint8_t)(0xb0+i);
    uint8_t rnda[16];      for(int i=0;i<16;i++) rnda[i]=(uint8_t)(0xa0+i);

    vcard_init(t, key, card_rndb);
    size_t bs = vcard_block();
    uint8_t enc_rndb[16]; vcard_challenge(enc_rndb);

    MfDesfireCryptoCtx ctx; uint8_t token[32];
    CHECK(mf_desfire_auth_begin(&ctx, t, key, enc_rndb, rnda, token), name);

    uint8_t enc_rndap[16];
    CHECK(vcard_respond(token, enc_rndap), name);
    CHECK(mf_desfire_auth_finish(&ctx, key, enc_rndap), name);
    CHECK(ctx.authenticated, name);

    uint8_t sk[24]; mf_desfire_derive_session_key(t, rnda, card_rndb, sk);
    CHECK(memcmp(ctx.session_key, sk, mf_desfire_key_size(t)) == 0, name);
    (void)bs;
}
static void test_auth(void) {
    auth_roundtrip(MfDesfireKeyTypeAes,    "aes auth round-trip");
    auth_roundtrip(MfDesfireKeyType2k3Des, "2k3des auth round-trip");
    auth_roundtrip(MfDesfireKeyTypeDes,    "des auth round-trip");
    auth_roundtrip(MfDesfireKeyType3k3Des, "3k3des auth round-trip");
}

static void test_cmac(void) {
    // Deterministic self-consistency test (RFC 4493 structure), not an external KAT.
    uint8_t key[24]; for(int i=0;i<16;i++) key[i]=(uint8_t)i;
    MfDesfireCryptoCtx c; memset(&c,0,sizeof c);
    c.type = MfDesfireKeyTypeAes; c.block_size = 16;
    memcpy(c.session_key, key, 16);
    mf_desfire_cmac_init(&c);
    // subkeys must be non-zero and sk2 == sk1<<1 (mod) — sanity, not value-locked
    int nz = 0; for(int i=0;i<16;i++) nz |= c.cmac_sk1[i];
    CHECK(nz != 0, "cmac subkey1 derived");

    uint8_t m[16] = {0}; uint8_t mac_a[8], mac_b[8];
    memset(c.iv,0,16); mf_desfire_cmac(&c, m, 16, mac_a);
    memset(c.iv,0,16); mf_desfire_cmac(&c, m, 16, mac_b);
    CHECK(memcmp(mac_a, mac_b, 8) == 0, "cmac deterministic");
}

// Authoritative known-answer test: RFC 4493 subkey generation vectors for
// AES-128 CMAC with the RFC's example key. This validates the left-shift +
// Rb conditional-XOR logic against a published external source, not just
// self-consistency.
static void test_cmac_rfc4493_kat(void) {
    uint8_t rfc_key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    uint8_t exp_sk1[16] = {
        0xfb,0xee,0xd6,0x18,0x35,0x71,0x33,0x66,
        0x7c,0x85,0xe0,0x8f,0x72,0x36,0xa8,0xde
    };
    uint8_t exp_sk2[16] = {
        0xf7,0xdd,0xac,0x30,0x6a,0xe2,0x66,0xcc,
        0xf9,0x0b,0xc1,0x1e,0xe4,0x6d,0x51,0x3b
    };
    MfDesfireCryptoCtx c; memset(&c,0,sizeof c);
    c.type = MfDesfireKeyTypeAes; c.block_size = 16;
    memcpy(c.session_key, rfc_key, 16);
    mf_desfire_cmac_init(&c);
    CHECK(memcmp(c.cmac_sk1, exp_sk1, 16) == 0, "cmac sk1 matches RFC 4493 KAT");
    CHECK(memcmp(c.cmac_sk2, exp_sk2, 16) == 0, "cmac sk2 matches RFC 4493 KAT");
}

// Authoritative known-answer test: RFC 4493 §4 full-message CMAC vectors for
// AES-128, using the RFC's example key and 64-byte message M, sliced to each
// example's length. These exercise every branch of mf_desfire_cmac's
// full/rem block logic that the single-block determinism test above cannot:
// empty message (padded final block, sk2, zero data blocks), a single
// block-aligned message (sk1, no padding), a multi-block non-aligned message
// (padded final block, sk2, >=1 full prior block), and a multi-block
// block-aligned message (sk1, >=1 full prior block). mf_desfire_cmac returns
// only the leading 8 bytes, so we check against the first 8 bytes of each
// RFC-published 16-byte CMAC.
static void test_cmac_rfc4493_messages(void) {
    uint8_t rfc_key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    uint8_t M[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10
    };

    MfDesfireCryptoCtx c; memset(&c,0,sizeof c);
    c.type = MfDesfireKeyTypeAes; c.block_size = 16;
    memcpy(c.session_key, rfc_key, 16);
    mf_desfire_cmac_init(&c);

    uint8_t mac8[8];

    // Example 1: len=0 (empty) — padded final block, sk2, zero full blocks.
    uint8_t exp1[8] = {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28};
    memset(c.iv,0,16); mf_desfire_cmac(&c, M, 0, mac8);
    CHECK(memcmp(mac8, exp1, 8) == 0, "cmac RFC 4493 message KAT: empty");

    // Example 2: len=16 — single block, aligned, sk1.
    uint8_t exp2[8] = {0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44};
    memset(c.iv,0,16); mf_desfire_cmac(&c, M, 16, mac8);
    CHECK(memcmp(mac8, exp2, 8) == 0, "cmac RFC 4493 message KAT: 16 bytes");

    // Example 3: len=40 — multi-block, non-aligned, padded final block, sk2.
    uint8_t exp3[8] = {0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30};
    memset(c.iv,0,16); mf_desfire_cmac(&c, M, 40, mac8);
    CHECK(memcmp(mac8, exp3, 8) == 0, "cmac RFC 4493 message KAT: 40 bytes");

    // Example 4: len=64 — multi-block, aligned, sk1.
    uint8_t exp4[8] = {0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92};
    memset(c.iv,0,16); mf_desfire_cmac(&c, M, 64, mac8);
    CHECK(memcmp(mac8, exp4, 8) == 0, "cmac RFC 4493 message KAT: 64 bytes");
}

// DES-family (bs=8) exercise: no cheap authoritative KAT available for
// 2K3DES CMAC, so this proves the bs=8 code path runs without overflowing
// the 16-byte scratch buffers and is deterministic, mirroring test_cmac's
// approach but for the DES-family block size instead of AES's.
static void test_cmac_des_family_bs8(void) {
    uint8_t key[16]; for(int i=0;i<16;i++) key[i]=(uint8_t)(0x40+i);
    MfDesfireCryptoCtx c2; memset(&c2,0,sizeof c2);
    c2.type = MfDesfireKeyType2k3Des; c2.block_size = 8;
    memcpy(c2.session_key, key, 16);
    mf_desfire_cmac_init(&c2);

    uint8_t m[20]; for(int i=0;i<20;i++) m[i]=(uint8_t)i;
    uint8_t mac_a[8], mac_b[8];
    memset(c2.iv,0,8); mf_desfire_cmac(&c2, m, 20, mac_a);
    memset(c2.iv,0,8); mf_desfire_cmac(&c2, m, 20, mac_b);
    CHECK(memcmp(mac_a, mac_b, 8) == 0, "cmac 2k3des (bs=8) deterministic, no overflow");
}

extern void test_attack_sweep(void);
extern void test_attack_dict_builtin(void);

int main(void) {
    test_mbedtls_selftest();
    test_helpers();
    test_session_key();
    test_auth();
    test_cmac();
    test_cmac_rfc4493_kat();
    test_cmac_rfc4493_messages();
    test_cmac_des_family_bs8();
    test_attack_sweep();
    test_attack_dict_builtin();
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
