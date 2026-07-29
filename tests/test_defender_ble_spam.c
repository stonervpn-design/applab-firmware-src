#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Include the pure header.
#include "../applications/main/defender/defender_ble_spam.h"

/**
 * Test: Continuity count below threshold → no alert.
 */
static void test_continuity_below(void) {
    DefenderBleSpamRec recs[24];
    uint8_t base_addr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};

    // 24 Continuity adverts at same MAC.
    for(int i = 0; i < 24; i++) {
        recs[i].addr[0] = base_addr[0];
        recs[i].addr[1] = base_addr[1];
        recs[i].addr[2] = base_addr[2];
        recs[i].addr[3] = base_addr[3];
        recs[i].addr[4] = base_addr[4];
        recs[i].addr[5] = base_addr[5];
        recs[i].is_random_mac = false;
        recs[i].company_id = BLE_CID_APPLE;
        recs[i].service_uuid = 0;
        recs[i].ts_ms = 49500 + i;
    }

    DefenderBleSpamResult out;
    defender_blespam_eval(recs, 24, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.continuity == 24);
    assert(out.fastpair == 0);
    assert(out.swiftpair == 0);
    assert(out.distinct_random_macs == 0);
    assert(out.alert == false);  // 24 < 25
    printf("✓ test_continuity_below passed\n");
}

/**
 * Test: Continuity count at threshold → alert.
 */
static void test_continuity_at(void) {
    DefenderBleSpamRec recs[25];
    uint8_t base_addr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};

    // 25 Continuity adverts.
    for(int i = 0; i < 25; i++) {
        recs[i].addr[0] = base_addr[0];
        recs[i].addr[1] = base_addr[1];
        recs[i].addr[2] = base_addr[2];
        recs[i].addr[3] = base_addr[3];
        recs[i].addr[4] = base_addr[4];
        recs[i].addr[5] = base_addr[5];
        recs[i].is_random_mac = false;
        recs[i].company_id = BLE_CID_APPLE;
        recs[i].service_uuid = 0;
        recs[i].ts_ms = 49500 + i;
    }

    DefenderBleSpamResult out;
    defender_blespam_eval(recs, 25, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.continuity == 25);
    assert(out.fastpair == 0);
    assert(out.swiftpair == 0);
    assert(out.distinct_random_macs == 0);
    assert(out.alert == true);  // 25 >= 25
    printf("✓ test_continuity_at passed\n");
}

/**
 * Test: Mixed payload types (Continuity + FastPair + SwiftPair) → alert.
 */
static void test_mixed_payload(void) {
    DefenderBleSpamRec recs[25];
    int idx = 0;

    // 10 Continuity.
    for(int i = 0; i < 10; i++) {
        recs[idx].addr[0] = 0xAA;
        recs[idx].addr[1] = 0xBB;
        recs[idx].addr[2] = 0xCC;
        recs[idx].addr[3] = 0xDD;
        recs[idx].addr[4] = 0xEE;
        recs[idx].addr[5] = 0x01 + i;
        recs[idx].is_random_mac = false;
        recs[idx].company_id = BLE_CID_APPLE;
        recs[idx].service_uuid = 0;
        recs[idx].ts_ms = 49500 + i;
        idx++;
    }

    // 10 FastPair.
    for(int i = 0; i < 10; i++) {
        recs[idx].addr[0] = 0xBB;
        recs[idx].addr[1] = 0xCC;
        recs[idx].addr[2] = 0xDD;
        recs[idx].addr[3] = 0xEE;
        recs[idx].addr[4] = 0xFF;
        recs[idx].addr[5] = 0x01 + i;
        recs[idx].is_random_mac = false;
        recs[idx].company_id = 0;
        recs[idx].service_uuid = BLE_UUID_FASTPAIR;
        recs[idx].ts_ms = 49510 + i;
        idx++;
    }

    // 5 SwiftPair.
    for(int i = 0; i < 5; i++) {
        recs[idx].addr[0] = 0xCC;
        recs[idx].addr[1] = 0xDD;
        recs[idx].addr[2] = 0xEE;
        recs[idx].addr[3] = 0xFF;
        recs[idx].addr[4] = 0x00;
        recs[idx].addr[5] = 0x01 + i;
        recs[idx].is_random_mac = false;
        recs[idx].company_id = BLE_CID_MICROSOFT;
        recs[idx].service_uuid = 0;
        recs[idx].ts_ms = 49520 + i;
        idx++;
    }

    DefenderBleSpamResult out;
    defender_blespam_eval(recs, 25, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.continuity == 10);
    assert(out.fastpair == 10);
    assert(out.swiftpair == 5);
    assert(out.distinct_random_macs == 0);
    assert(out.alert == true);  // 10+10+5 = 25 >= 25
    printf("✓ test_mixed_payload passed\n");
}

/**
 * Test: Random MAC flood → alert.
 */
static void test_mac_flood(void) {
    DefenderBleSpamRec recs[40];

    // 40 records, each with distinct random MAC.
    for(int i = 0; i < 40; i++) {
        recs[i].addr[0] = 0x02 | (i & 0xFE);  // Locally-administered bit set.
        recs[i].addr[1] = i;
        recs[i].addr[2] = 0xFF;
        recs[i].addr[3] = 0x00;
        recs[i].addr[4] = 0x11;
        recs[i].addr[5] = 0x22;
        recs[i].is_random_mac = true;
        recs[i].company_id = 0;  // Benign payload.
        recs[i].service_uuid = 0;
        recs[i].ts_ms = 49500 + i;
    }

    DefenderBleSpamResult out;
    defender_blespam_eval(recs, 40, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.continuity == 0);
    assert(out.fastpair == 0);
    assert(out.swiftpair == 0);
    assert(out.distinct_random_macs == 40);
    assert(out.alert == true);  // 40 >= 40
    printf("✓ test_mac_flood passed\n");
}

/**
 * Test: Same random MAC not double-counted → no alert.
 */
static void test_mac_not_double_counted(void) {
    DefenderBleSpamRec recs[40];
    uint8_t random_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

    // 40 records, all same random MAC.
    for(int i = 0; i < 40; i++) {
        memcpy(recs[i].addr, random_mac, 6);
        recs[i].is_random_mac = true;
        recs[i].company_id = 0;
        recs[i].service_uuid = 0;
        recs[i].ts_ms = 49500 + i;
    }

    DefenderBleSpamResult out;
    defender_blespam_eval(recs, 40, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.distinct_random_macs == 1);  // Only 1 unique MAC.
    assert(out.alert == false);  // 1 < 40
    printf("✓ test_mac_not_double_counted passed\n");
}

/**
 * Test: Window sliding excludes stale records.
 */
static void test_window_slide(void) {
    DefenderBleSpamRec recs[35];
    int idx = 0;

    // 30 Continuity at ts 48000 (stale: older than window_ms).
    for(int i = 0; i < 30; i++) {
        recs[idx].addr[0] = 0xAA;
        recs[idx].addr[1] = 0xBB;
        recs[idx].addr[2] = 0xCC;
        recs[idx].addr[3] = 0xDD;
        recs[idx].addr[4] = 0xEE;
        recs[idx].addr[5] = 0x01 + i;
        recs[idx].is_random_mac = false;
        recs[idx].company_id = BLE_CID_APPLE;
        recs[idx].service_uuid = 0;
        recs[idx].ts_ms = 48000 + i;
        idx++;
    }

    // 5 Continuity at ts 49500 (recent: within window).
    for(int i = 0; i < 5; i++) {
        recs[idx].addr[0] = 0xBB;
        recs[idx].addr[1] = 0xCC;
        recs[idx].addr[2] = 0xDD;
        recs[idx].addr[3] = 0xEE;
        recs[idx].addr[4] = 0xFF;
        recs[idx].addr[5] = 0x01 + i;
        recs[idx].is_random_mac = false;
        recs[idx].company_id = BLE_CID_APPLE;
        recs[idx].service_uuid = 0;
        recs[idx].ts_ms = 49500 + i;
        idx++;
    }

    DefenderBleSpamResult out;
    // Window: (50000 - 1000, 50000] = (49000, 50000].
    // 48000 + i (i < 30) → max 48029, all < 49000 (stale).
    // 49500 + i (i < 5) → range [49500, 49504], all in (49000, 50000].
    defender_blespam_eval(recs, 35, 50000, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &out);

    assert(out.continuity == 5);  // Only recent 5 counted.
    assert(out.alert == false);  // 5 < 25
    printf("✓ test_window_slide passed\n");
}

/**
 * Main entry point: run all tests.
 */
int main(void) {
    test_continuity_below();
    test_continuity_at();
    test_mixed_payload();
    test_mac_flood();
    test_mac_not_double_counted();
    test_window_slide();

    printf("\nALL DEFENDER BLE SPAM TESTS PASSED\n");
    return 0;
}
