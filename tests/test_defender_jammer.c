#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

// Include the jammer header
#include "../applications/main/defender/defender_jammer.h"

/**
 * test_floor_tracks: Feed constant RSSI samples and verify floor converges.
 */
static void test_floor_tracks(void) {
    printf("TEST: floor_tracks\n");
    DefenderJammerState st = {0};

    // Feed -95.0 dBm repeatedly (no packet decoded)
    for(uint32_t t = 0; t <= 1000; t += 100) {
        defender_jammer_feed(&st, -95.0f, false, t);
    }

    // Floor should converge close to -95 dBm
    printf("  floor_ema = %.2f (expect ~-95.0)\n", st.floor_ema);
    assert(st.have_floor);
    assert(fabsf(st.floor_ema - (-95.0f)) < 1.0f);
    assert(st.jamming == false);
    printf("  PASS\n");
}

/**
 * test_jam: Sustained energy above floor triggers jam alert.
 */
static void test_jam(void) {
    printf("TEST: jam\n");
    DefenderJammerState st = {0};

    // Prime the floor to ~-95 dBm
    for(int i = 0; i < 5; i++) {
        defender_jammer_feed(&st, -95.0f, false, i * 100);
    }
    printf("  floor primed to %.2f\n", st.floor_ema);

    // Now feed -60 dBm (35 dB over threshold of -77 dBm) with no packet decoded
    // t=2000, 2100, 2200, 2300 → not yet 400 ms, jamming should be false
    defender_jammer_feed(&st, -60.0f, false, 2000);
    assert(st.jamming == false);

    defender_jammer_feed(&st, -60.0f, false, 2100);
    assert(st.jamming == false);

    defender_jammer_feed(&st, -60.0f, false, 2200);
    assert(st.jamming == false);

    defender_jammer_feed(&st, -60.0f, false, 2300);
    assert(st.jamming == false);

    // At t=2400, we've been over-threshold for 400 ms → jamming should be true
    defender_jammer_feed(&st, -60.0f, false, 2400);
    printf("  jamming at t=2400: %s (expect true)\n", st.jamming ? "true" : "false");
    assert(st.jamming == true);
    printf("  PASS\n");
}

/**
 * test_spike_with_packet: Real transmitter (packet decoded) doesn't trigger jam.
 */
static void test_spike_with_packet(void) {
    printf("TEST: spike_with_packet\n");
    DefenderJammerState st = {0};

    // Prime floor to ~-95
    for(int i = 0; i < 5; i++) {
        defender_jammer_feed(&st, -95.0f, false, i * 100);
    }

    // Feed -60 dBm with packet_decoded=true (legitimate transmitter)
    for(uint32_t t = 3000; t <= 3500; t += 100) {
        defender_jammer_feed(&st, -60.0f, true, t);
    }

    printf("  jamming: %s (expect false, legitimate packet)\n", st.jamming ? "true" : "false");
    printf("  over_since_ms: %u (expect 0, reset by packet_decoded)\n", st.over_since_ms);
    assert(st.jamming == false);
    assert(st.over_since_ms == 0);
    printf("  PASS\n");
}

/**
 * test_brief_spike_no_packet: Brief spike (< 400 ms) doesn't trigger jam.
 */
static void test_brief_spike_no_packet(void) {
    printf("TEST: brief_spike_no_packet\n");
    DefenderJammerState st = {0};

    // Prime floor to ~-95
    for(int i = 0; i < 5; i++) {
        defender_jammer_feed(&st, -95.0f, false, i * 100);
    }

    // Feed -60 dBm at t=4000 (over-threshold starts)
    defender_jammer_feed(&st, -60.0f, false, 4000);
    assert(st.over_since_ms == 4000);

    // Energy drops below threshold at t=4200 (only 200 ms of over-threshold)
    defender_jammer_feed(&st, -90.0f, false, 4200);

    printf("  jamming: %s (expect false, brief spike)\n", st.jamming ? "true" : "false");
    printf("  over_since_ms: %u (expect 0, reset by drop below threshold)\n", st.over_since_ms);
    assert(st.jamming == false);
    assert(st.over_since_ms == 0);
    printf("  PASS\n");
}

int main(void) {
    printf("Running Defender Jammer tests...\n\n");

    test_floor_tracks();
    test_jam();
    test_spike_with_packet();
    test_brief_spike_no_packet();

    printf("\nALL DEFENDER JAMMER TESTS PASSED\n");
    return 0;
}
