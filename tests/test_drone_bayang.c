// tests/test_drone_bayang.c
// Host unit test for the Bayang toy-quadcopter nRF24 packet builder
// (ND1 Task 2). Compile+run under WSL:
//   wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" &&
//     gcc -std=gnu11 -Wall -Wextra -fsanitize=address -Iapplications/main/nrf24/helpers
//     tests/test_drone_bayang.c applications/main/nrf24/helpers/drone_bayang.c
//     -o /tmp/tdb && /tmp/tdb'
//
// These tests prove INTERNAL CONSISTENCY only (checksum determinism, field
// packing, channel bounds, clamping) -- NOT real-frame correctness against
// an actual Bayang drone/receiver. See drone_bayang.h for the UNVERIFIED
// status of the transcribed wire format.

#include "drone_bayang.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_checksum_deterministic(void) {
    uint8_t p[BAYANG_PKT_LEN]; memset(p, 0, sizeof(p));
    for(int i = 0; i < 14; i++) p[i] = (uint8_t)(i * 7 + 1);
    uint8_t a = drone_bayang_checksum(p, BAYANG_PKT_LEN);
    uint8_t b = drone_bayang_checksum(p, BAYANG_PKT_LEN);
    assert(a == b);                 /* deterministic */
    p[3] ^= 0xFF;
    assert(drone_bayang_checksum(p, BAYANG_PKT_LEN) != a); /* covers the payload */
    printf("test_checksum_deterministic OK\n");
}

static void test_control_packing_and_checksum(void) {
    uint8_t txid[3] = {0x11, 0x22, 0x33};
    uint8_t out[BAYANG_PKT_LEN];
    drone_bayang_build_control(txid, 0x155, 0x200, 0x200, 0x200, 0x00, out);
    assert(out[0] == 0xA5);                              /* control flags/normal byte */
    assert(out[11] == 0x11 && out[12] == 0x22 && out[13] == 0x33); /* txid */
    assert(out[14] == drone_bayang_checksum(out, BAYANG_PKT_LEN)); /* checksum trails */
    /* deterministic */
    uint8_t out2[BAYANG_PKT_LEN];
    drone_bayang_build_control(txid, 0x155, 0x200, 0x200, 0x200, 0x00, out2);
    assert(memcmp(out, out2, BAYANG_PKT_LEN) == 0);
    printf("test_control_packing_and_checksum OK\n");
}

static void test_bind_differs_from_control(void) {
    uint8_t txid[3] = {1, 2, 3};
    uint8_t bind[BAYANG_PKT_LEN], ctrl[BAYANG_PKT_LEN];
    drone_bayang_build_bind(txid, bind);
    drone_bayang_build_control(txid, 0, 0x200, 0x200, 0x200, 0, ctrl);
    assert(bind[0] == 0xA4);          /* bind byte */
    assert(bind[0] != ctrl[0]);       /* bind vs control distinguishable */
    printf("test_bind_differs_from_control OK\n");
}

static void test_channels_in_band(void) {
    for(int t = 0; t < 256; t++) {
        uint8_t txid[3] = {(uint8_t)t, (uint8_t)(t * 3), (uint8_t)(t * 7)};
        uint8_t ch[BAYANG_RF_NUM_CH];
        drone_bayang_rf_channels(txid, ch);
        for(int i = 0; i < BAYANG_RF_NUM_CH; i++) assert(ch[i] <= 125);
    }
    printf("test_channels_in_band OK\n");
}

static void test_axis_clamp_no_overflow(void) {
    uint8_t txid[3] = {0, 0, 0};
    uint8_t out[BAYANG_PKT_LEN];
    /* absurd inputs must clamp, never overflow the packet (ASAN catches OOB) */
    drone_bayang_build_control(txid, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, out);
    assert(out[14] == drone_bayang_checksum(out, BAYANG_PKT_LEN));
    printf("test_axis_clamp_no_overflow OK\n");
}

int main(void) {
    test_checksum_deterministic();
    test_control_packing_and_checksum();
    test_bind_differs_from_control();
    test_channels_in_band();
    test_axis_clamp_no_overflow();
    printf("test_drone_bayang: ALL PASS\n");
    return 0;
}
