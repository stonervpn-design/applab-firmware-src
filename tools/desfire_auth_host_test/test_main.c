// tools/desfire_auth_host_test/test_main.c
#include <mbedtls/aes.h>
#include <mbedtls/des.h>
#include <stdio.h>

static int g_fail = 0;
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

int main(void) {
    test_mbedtls_selftest();
    test_helpers();
    test_session_key();
    test_auth();
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
