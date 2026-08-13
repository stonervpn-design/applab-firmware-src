#include "wlan_wps_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* IE region: an SSID IE then a WPS vendor IE. WPS attrs: Version 1.0,
 * State=configured, AP-Setup-Locked=<lock>, Config-Methods=<cm hi,lo>. */
static uint16_t build_ies(uint8_t* b, uint8_t lock, uint8_t cm_hi, uint8_t cm_lo) {
    uint16_t n = 0;
    /* SSID IE (id 0, len 2) */
    b[n++] = 0x00; b[n++] = 0x02; b[n++] = 'H'; b[n++] = 'i';
    /* WPS vendor IE */
    b[n++] = 0xDD;               /* vendor specific */
    uint16_t lenpos = n++;       /* length filled below */
    b[n++] = 0x00; b[n++] = 0x50; b[n++] = 0xF2; b[n++] = 0x04; /* OUI + WPS type */
    b[n++] = 0x10; b[n++] = 0x4A; b[n++] = 0x00; b[n++] = 0x01; b[n++] = 0x10; /* Version 1.0 */
    b[n++] = 0x10; b[n++] = 0x44; b[n++] = 0x00; b[n++] = 0x01; b[n++] = 0x02; /* State=configured */
    b[n++] = 0x10; b[n++] = 0x57; b[n++] = 0x00; b[n++] = 0x01; b[n++] = lock; /* AP-Setup-Locked */
    b[n++] = 0x10; b[n++] = 0x08; b[n++] = 0x00; b[n++] = 0x02; b[n++] = cm_hi; b[n++] = cm_lo; /* Config Methods */
    b[lenpos] = (uint8_t)(n - lenpos - 1); /* IE length = bytes after the length field */
    return n;
}

static void test_present_unlocked_pbc(void) {
    uint8_t b[128]; uint16_t n = build_ies(b, 0x00, 0x00, 0x80); /* unlocked, PBC */
    WlanWpsInfo w;
    assert(wlan_wps_parse_ies(b, n, &w) == true);
    assert(w.present && !w.locked);
    assert(w.config_methods == 0x0080);
    assert(w.version == 0x10 && w.wps_state == 0x02);
    printf("test_present_unlocked_pbc OK\n");
}

static void test_present_locked_pin(void) {
    uint8_t b[128]; uint16_t n = build_ies(b, 0x01, 0x01, 0x00); /* locked, Keypad 0x0100 */
    WlanWpsInfo w;
    assert(wlan_wps_parse_ies(b, n, &w));
    assert(w.present && w.locked);
    assert(w.config_methods == 0x0100);
    printf("test_present_locked_pin OK\n");
}

static void test_no_wps(void) {
    uint8_t b[8] = {0x00, 0x02, 'H', 'i', 0x01, 0x01, 0x82, 0x00}; /* SSID + rates, no WPS */
    WlanWpsInfo w;
    assert(wlan_wps_parse_ies(b, 8, &w) == false);
    assert(!w.present);
    printf("test_no_wps OK\n");
}

static void test_other_vendor_ie_not_wps(void) {
    /* 0xDD vendor IE with Microsoft WMM (OUI 00:50:F2 TYPE 0x02) -> NOT WPS */
    uint8_t b[9] = {0xDD, 0x07, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00};
    WlanWpsInfo w;
    assert(wlan_wps_parse_ies(b, 9, &w) == false);
    printf("test_other_vendor_ie_not_wps OK\n");
}

static void test_truncated_no_oob(void) {
    uint8_t b[128]; uint16_t n = build_ies(b, 0x00, 0x00, 0x80);
    /* Feed only a prefix so the WPS IE / an attribute is cut mid-way. */
    for(uint16_t cut = 1; cut < n; cut++) {
        WlanWpsInfo w;
        (void)wlan_wps_parse_ies(b, cut, &w); /* must not crash / read OOB (ASAN would catch) */
    }
    /* An IE claiming a length past the buffer must not over-read. */
    uint8_t bad[6] = {0xDD, 0x40, 0x00, 0x50, 0xF2, 0x04}; /* len 0x40 but only 4 bytes follow */
    WlanWpsInfo w2;
    assert(wlan_wps_parse_ies(bad, 6, &w2) == false);
    printf("test_truncated_no_oob OK\n");
}

int main(void) {
    test_present_unlocked_pbc();
    test_present_locked_pin();
    test_no_wps();
    test_other_vendor_ie_not_wps();
    test_truncated_no_oob();
    printf("test_wlan_wps_parse: ALL PASS\n");
    return 0;
}
