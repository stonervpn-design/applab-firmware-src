#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/whisperpair_fp.h"

static void test_discoverable(void) {
    // AD field: len=0x06, type=0x16 (svc data 16-bit), uuid 2C FE (LE), model AA BB CC
    uint8_t adv[] = {0x06, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC};
    uint32_t model = 0;
    assert(fp_adv_parse(adv, sizeof(adv), &model) == FpAdvDiscoverable);
    assert(model == 0xAABBCCu); // big-endian: octet0 MSB
    printf("test_discoverable OK\n");
}

static void test_has_accounts(void) {
    // 0xFE2C service data, 5-byte payload (flags + 4) -> account-key filter
    uint8_t adv[] = {0x08, 0x16, 0x2C, 0xFE, 0x00, 0x11, 0x22, 0x33, 0x44};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvHasAccounts);
    printf("test_has_accounts OK\n");
}

static void test_none(void) {
    // Flags AD (0x01) then service data for a DIFFERENT uuid (0xFEAA)
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x05, 0x16, 0xAA, 0xFE, 0x01, 0x02};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvNone);
    printf("test_none OK\n");
}

static void test_malformed(void) {
    // field length 0x20 overruns a 4-byte buffer -> no over-read, FpAdvNone
    uint8_t adv[] = {0x20, 0x16, 0x2C, 0xFE};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvNone);
    // zero-length field terminates the walk
    uint8_t adv2[] = {0x00, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC};
    assert(fp_adv_parse(adv2, sizeof(adv2), NULL) == FpAdvNone);
    printf("test_malformed OK\n");
}

static void test_name_match(void) {
    assert(fp_name_is_vuln("Sony WH-1000XM5") == true);
    assert(fp_name_is_vuln("wh-1000xm5") == true);          // case-insensitive
    assert(fp_name_is_vuln("Pixel Buds Pro 2") == true);
    assert(fp_name_is_vuln("My Random Speaker") == false);
    assert(fp_name_is_vuln("") == false);
    assert(fp_name_is_vuln(NULL) == false);
    printf("test_name_match OK\n");
}

int main(void) {
    test_discoverable();
    test_has_accounts();
    test_none();
    test_malformed();
    test_name_match();
    printf("ALL WHISPERPAIR FP TESTS PASSED\n");
    return 0;
}
