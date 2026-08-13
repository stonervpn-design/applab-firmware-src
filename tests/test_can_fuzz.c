#include "can_fuzz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CanFrame mk_base(void) {
    CanFrame b; memset(&b, 0, sizeof(b));
    b.id = 0x123; b.extended = false; b.rtr = false; b.dlc = 8;
    for(int i = 0; i < 8; i++) b.data[i] = (uint8_t)(i + 1);
    return b;
}

static int popcount8x(const uint8_t* a, const uint8_t* b, int n) {
    int d = 0;
    for(int i = 0; i < n; i++) { uint8_t x = a[i]^b[i]; while(x){d+=x&1;x>>=1;} }
    return d;
}

static void test_valid_and_deterministic(void) {
    CanFrame b = mk_base();
    for(int m = 0; m < CFZ_MODE_COUNT; m++) {
        CanFrame x, y;
        can_fuzz_next(&b, (CanFuzzMode)m, 0xABCD, 11, &x);
        can_fuzz_next(&b, (CanFuzzMode)m, 0xABCD, 11, &y);
        assert(memcmp(&x, &y, sizeof(CanFrame)) == 0);       /* deterministic */
        assert(x.dlc <= 8);                                   /* TWAI-valid dlc */
        uint32_t max = x.extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
        assert(x.id <= max);                                  /* TWAI-valid id */
    }
    printf("test_valid_and_deterministic OK\n");
}

static void test_bitflip_one_data_bit(void) {
    CanFrame b = mk_base(), x;
    can_fuzz_next(&b, CFZ_DATA_BITFLIP, 0x7, 3, &x);
    assert(x.id == b.id && x.dlc == b.dlc && x.extended == b.extended);
    assert(popcount8x(x.data, b.data, b.dlc) == 1);          /* exactly one data bit */
    printf("test_bitflip_one_data_bit OK\n");
}

static void test_id_sweep_bounds(void) {
    CanFrame b = mk_base(); /* std */
    for(uint32_t s = 0; s < 5000; s++) {
        CanFrame x; can_fuzz_next(&b, CFZ_ID_SWEEP, 0, s, &x);
        assert(x.id <= CAN_ID_STD_MAX);
    }
    CanFrame be = mk_base(); be.extended = true;
    for(uint32_t s = 0; s < 5000; s++) {
        CanFrame x; can_fuzz_next(&be, CFZ_ID_SWEEP, 0, s, &x);
        assert(x.id <= CAN_ID_EXT_MAX);
    }
    printf("test_id_sweep_bounds OK\n");
}

static void test_dlc_sweep_cycle(void) {
    CanFrame b = mk_base();
    for(uint32_t s = 0; s <= 8; s++) {
        CanFrame x; can_fuzz_next(&b, CFZ_DLC_SWEEP, 0, s, &x);
        assert(x.dlc == (s % 9));
    }
    printf("test_dlc_sweep_cycle OK\n");
}

static void test_null_guards(void) {
    CanFrame b = mk_base(), x;
    can_fuzz_next(NULL, CFZ_DATA_RANDOM, 0, 0, &x); /* no crash */
    can_fuzz_next(&b, CFZ_DATA_RANDOM, 0, 0, NULL); /* no crash */
    printf("test_null_guards OK\n");
}

int main(void) {
    test_valid_and_deterministic();
    test_bitflip_one_data_bit();
    test_id_sweep_bounds();
    test_dlc_sweep_cycle();
    test_null_guards();
    printf("test_can_fuzz: ALL PASS\n");
    return 0;
}
