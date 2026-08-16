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

int main(void) {
    test_mbedtls_selftest();
    test_helpers();
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
