#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* Include the pure classifier from the tap header */
#include "../applications/main/defender/defender_wifi_tap.h"

/* Helper to build a test frame */
static void build_frame(uint8_t* p, uint8_t fc0, uint8_t fc1) {
    p[0] = fc0;
    p[1] = fc1;
    p[2] = 0; p[3] = 0;  /* duration/ID */
    /* addr1 at [4..9], addr2 at [10..15], addr3 at [16..21] */
}

static void set_addr(uint8_t* p, int offset, const uint8_t addr[6]) {
    memcpy(&p[offset], addr, 6);
}

static void set_reason(uint8_t* p, uint16_t reason) {
    p[24] = (uint8_t)(reason & 0xFF);
    p[25] = (uint8_t)((reason >> 8) & 0xFF);
}

int main(void) {
    uint8_t frame[26];
    DefWifiFrame out;

    /* Test 1: Deauth frame (type=0, subtype=0x0C) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xC0, 0x00);
    uint8_t addr_test[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    set_addr(frame, 4, addr_test);   /* addr1 */
    uint8_t addr2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    set_addr(frame, 10, addr2);      /* addr2 */
    set_addr(frame, 16, addr_test);  /* addr3 (bssid for mgmt) */
    set_reason(frame, 7);            /* reason = 7 */

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiDeauth);
    assert(memcmp(out.addr1, addr_test, 6) == 0);
    assert(memcmp(out.addr2, addr2, 6) == 0);
    assert(memcmp(out.addr3, addr_test, 6) == 0);
    assert(out.reason_code == 7);
    assert(out.broadcast_addr1 == false);
    printf("Test 1 (Deauth): PASS\n");

    /* Test 2: Disassoc frame (type=0, subtype=0x0A) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xA0, 0x00);
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);
    set_reason(frame, 10);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiDisassoc);
    assert(out.reason_code == 10);
    printf("Test 2 (Disassoc): PASS\n");

    /* Test 3: Beacon frame (type=0, subtype=0x08) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0x80, 0x00);
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiBeacon);
    assert(out.reason_code == 0);
    printf("Test 3 (Beacon): PASS\n");

    /* Test 4: Probe Response (type=0, subtype=0x05) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0x50, 0x00);
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiProbeResp);
    printf("Test 4 (Probe Response): PASS\n");

    /* Test 5: Probe Request (type=0, subtype=0x04) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0x40, 0x00);
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiProbeReq);
    printf("Test 5 (Probe Request): PASS\n");

    /* Test 6: Data frame (type=2, subtype varies) → DefWifiOther */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0x08, 0x00);  /* type=2, subtype=0 */
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiOther);
    printf("Test 6 (Data frame): PASS\n");

    /* Test 7: Broadcast addr1 (deauth to all) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xC0, 0x00);
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    set_addr(frame, 4, bcast);       /* addr1 = broadcast */
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);
    set_reason(frame, 3);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiDeauth);
    assert(out.broadcast_addr1 == true);
    printf("Test 7 (Broadcast addr1): PASS\n");

    /* Test 8: Unicast addr1 (should not be broadcast) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xC0, 0x00);
    uint8_t unicast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};  /* not all FF */
    set_addr(frame, 4, unicast);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);
    set_reason(frame, 5);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.broadcast_addr1 == false);
    printf("Test 8 (Unicast addr1): PASS\n");

    /* Test 9: Short buffer (len < 24) → returns false, out untouched */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xC0, 0x00);
    DefWifiFrame out_before;
    memset(&out_before, 0xAA, sizeof(out_before));
    memcpy(&out, &out_before, sizeof(out));

    assert(defender_wifi_classify(frame, 20, &out) == false);
    assert(memcmp(&out, &out_before, sizeof(out)) == 0);  /* out untouched */
    printf("Test 9 (Short buffer): PASS\n");

    /* Test 10: Minimum length (26 bytes for reason code extraction) */
    memset(frame, 0, sizeof(frame));
    build_frame(frame, 0xA0, 0x00);  /* disassoc */
    set_addr(frame, 4, addr_test);
    set_addr(frame, 10, addr2);
    set_addr(frame, 16, addr_test);
    set_reason(frame, 0x1234);

    assert(defender_wifi_classify(frame, 26, &out) == true);
    assert(out.kind == DefWifiDisassoc);
    assert(out.reason_code == 0x1234);
    printf("Test 10 (26-byte frame): PASS\n");

    printf("\nALL DEFENDER WIFI FRAME TESTS PASSED\n");
    return 0;
}
