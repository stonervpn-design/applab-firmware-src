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

int main(void) {
    test_mbedtls_selftest();
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
