#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* Include the header to test */
#include "../applications/main/defender/defender_deauth.h"

/* Helper to create a test record */
static DefenderDeauthRec mk(uint8_t bssid_last, bool broadcast, uint32_t ts_ms) {
    DefenderDeauthRec rec = {0};
    rec.bssid[0] = 0xAA;
    rec.bssid[1] = 0xBB;
    rec.bssid[2] = 0xCC;
    rec.bssid[3] = 0xDD;
    rec.bssid[4] = 0xEE;
    rec.bssid[5] = bssid_last;
    rec.broadcast = broadcast;
    rec.rssi = -50;
    rec.ts_ms = ts_ms;
    return rec;
}

static void test_below(void) {
    DefenderDeauthRec recs[20];
    for(int i = 0; i < 19; i++) {
        recs[i] = mk(0xAA, false, 99500);
    }

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 19, 100000, 1000, 20, &res);

    assert(res.alert == false);
    assert(res.weighted_count == 19);
    assert(res.top_count == 19);
    printf("  test_below: PASS\n");
}

static void test_at(void) {
    DefenderDeauthRec recs[20];
    for(int i = 0; i < 20; i++) {
        recs[i] = mk(0xAA, false, 99500);
    }

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 20, 100000, 1000, 20, &res);

    assert(res.alert == true);
    assert(res.weighted_count == 20);
    assert(res.top_count == 20);
    printf("  test_at: PASS\n");
}

static void test_broadcast_weight(void) {
    DefenderDeauthRec recs[10];
    for(int i = 0; i < 10; i++) {
        recs[i] = mk(0xBB, true, 99500);
    }

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 10, 100000, 1000, 20, &res);

    /* 10 broadcast * 2 = 20 weighted */
    assert(res.weighted_count == 20);
    assert(res.alert == true);
    assert(res.top_count == 10);
    printf("  test_broadcast_weight: PASS\n");
}

static void test_broadcast_boundary(void) {
    DefenderDeauthRec recs[10];

    /* 9 broadcast (18 weighted) + 1 unicast (1) = 19 */
    for(int i = 0; i < 9; i++) {
        recs[i] = mk(0xCC, true, 99500);
    }
    recs[9] = mk(0xCC, false, 99500);

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 10, 100000, 1000, 20, &res);

    assert(res.weighted_count == 19);
    assert(res.alert == false);

    /* Now add one more broadcast (10 bc = 20) */
    recs[9] = mk(0xCC, true, 99500);

    res = (DefenderDeauthResult){0};
    defender_deauth_eval(recs, 10, 100000, 1000, 20, &res);

    assert(res.weighted_count == 20);
    assert(res.alert == true);
    printf("  test_broadcast_boundary: PASS\n");
}

static void test_window_slide(void) {
    DefenderDeauthRec recs[35];

    /* 30 recs at ts 98000 (older than now-window) */
    for(int i = 0; i < 30; i++) {
        recs[i] = mk(0xAA, false, 98000);
    }

    /* 5 recs at 99800 (within window) */
    for(int i = 30; i < 35; i++) {
        recs[i] = mk(0xBB, false, 99800);
    }

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 35, 100000, 1000, 20, &res);

    /* Only 5 should be in window */
    assert(res.weighted_count == 5);
    assert(res.alert == false);
    assert(res.top_count == 5);
    printf("  test_window_slide: PASS\n");
}

static void test_top_offender(void) {
    DefenderDeauthRec recs[21];

    /* 15 recs for BSSID A */
    for(int i = 0; i < 15; i++) {
        recs[i] = mk(0xAA, false, 99500);
    }

    /* 6 recs for BSSID B */
    for(int i = 15; i < 21; i++) {
        recs[i] = mk(0xBB, false, 99500);
    }

    DefenderDeauthResult res = {0};
    defender_deauth_eval(recs, 21, 100000, 1000, 20, &res);

    /* weighted_count should be 21, alert should be true */
    assert(res.weighted_count == 21);
    assert(res.alert == true);

    /* top offender should be A with count 15 */
    assert(res.top_count == 15);
    assert(res.top_bssid[5] == 0xAA);
    printf("  test_top_offender: PASS\n");
}

int main(void) {
    printf("Running DEFENDER DEAUTH tests...\n");
    test_below();
    test_at();
    test_broadcast_weight();
    test_broadcast_boundary();
    test_window_slide();
    test_top_offender();
    printf("ALL DEFENDER DEAUTH TESTS PASSED\n");
    return 0;
}
