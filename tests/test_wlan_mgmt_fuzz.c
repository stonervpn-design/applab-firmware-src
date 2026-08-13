#include "wlan_mgmt_fuzz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A 30-byte open-auth-ish base: 24-byte header + 6-byte body. */
static void mk_base(uint8_t* b, uint16_t* n) {
    memset(b, 0, 30);
    b[0] = 0xB0; /* mgmt, subtype auth */
    for(int i = 4; i < 22; i++) b[i] = (uint8_t)i; /* addr1/2/3 */
    *n = 30;
}

static void test_determinism(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    for(int m = 0; m < WMF_MODE_COUNT; m++) {
        uint8_t a[WLAN_FUZZ_MAX], b[WLAN_FUZZ_MAX]; uint16_t la, lb;
        assert(wlan_mgmt_fuzz_next(base, bn, (WlanFuzzMode)m, 0x1234, 7, a, &la));
        assert(wlan_mgmt_fuzz_next(base, bn, (WlanFuzzMode)m, 0x1234, 7, b, &lb));
        assert(la == lb && memcmp(a, b, la) == 0);
        assert(la >= 24 && la <= WLAN_FUZZ_MAX);
    }
    printf("test_determinism OK\n");
}

static void test_bitflip_one_bit(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo;
    assert(wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0xAA, 3, out, &lo));
    assert(lo == bn);
    int diff = 0;
    for(int i = 0; i < lo; i++) { uint8_t x = base[i] ^ out[i]; while(x){diff += x&1; x>>=1;} }
    assert(diff == 1); /* exactly one bit changed */
    printf("test_bitflip_one_bit OK\n");
}

static void test_lensweep_bounds(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    for(uint32_t s = 0; s < 400; s++) {
        uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo;
        assert(wlan_mgmt_fuzz_next(base, bn, WMF_LENSWEEP, 0, s, out, &lo));
        assert(lo >= 24 && lo <= WLAN_FUZZ_MAX);
    }
    printf("test_lensweep_bounds OK\n");
}

static void test_guards(void) {
    uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo; uint8_t base[30]; uint16_t bn; mk_base(base,&bn);
    assert(!wlan_mgmt_fuzz_next(NULL, bn, WMF_BITFLIP, 0, 0, out, &lo));
    assert(!wlan_mgmt_fuzz_next(base, 10, WMF_BITFLIP, 0, 0, out, &lo)); /* <24 */
    assert(!wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0, 0, NULL, &lo));
    assert(!wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0, 0, out, NULL));
    printf("test_guards OK\n");
}

/* IE_MANGLE at base_len==24 (no IE region): must be a safe same-length
 * passthrough (no div-by-zero, no mutation past the 24-byte header). */
static void test_ie_mangle_minlen_passthrough(void) {
    uint8_t base[24]; uint16_t bn = 24;
    memset(base, 0, sizeof(base));
    base[0] = 0xB0;
    for(int i = 4; i < 22; i++) base[i] = (uint8_t)i;
    uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo;
    assert(wlan_mgmt_fuzz_next(base, bn, WMF_IE_MANGLE, 0x55, 9, out, &lo));
    assert(lo == 24);
    assert(memcmp(out, base, 24) == 0); /* unchanged -- no IE bytes to mangle */
    printf("test_ie_mangle_minlen_passthrough OK\n");
}

int main(void) {
    test_determinism(); test_bitflip_one_bit(); test_lensweep_bounds(); test_guards();
    test_ie_mangle_minlen_passthrough();
    printf("test_wlan_mgmt_fuzz: ALL PASS\n");
    return 0;
}
