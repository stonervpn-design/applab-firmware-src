#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include <assert.h>
#include <stdio.h>

// Build a minimal probe-request: FC=0x40,0x00; dur; addr1(bcast); addr2(client);
// addr3(bcast); seq; then SSID IE (tag 0, len, bytes).
static uint16_t mk_probe(uint8_t* b, const uint8_t mac[6], const char* ssid) {
    uint16_t n = 0;
    b[n++] = 0x40; b[n++] = 0x00;              // frame control: mgmt probe-req
    b[n++] = 0x00; b[n++] = 0x00;              // duration
    for (int i = 0; i < 6; i++) b[n++] = 0xFF; // addr1
    for (int i = 0; i < 6; i++) b[n++] = mac[i]; // addr2 (client)
    for (int i = 0; i < 6; i++) b[n++] = 0xFF; // addr3
    b[n++] = 0x00; b[n++] = 0x00;              // seq ctl (offset 22..23)
    uint8_t sl = (uint8_t)strlen(ssid);        // tagged params @ offset 24
    b[n++] = 0x00; b[n++] = sl;
    for (uint8_t i = 0; i < sl; i++) b[n++] = (uint8_t)ssid[i];
    return n;
}

int main(void) {
    uint8_t buf[128];
    uint8_t mac[6] = {0xAA,0xBB,0xCC,0x11,0x22,0x33};
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t got[6];

    uint16_t len = mk_probe(buf, mac, "HomeNet");
    assert(wlan_probe_extract(buf, len, ssid, got));
    assert(strcmp(ssid, "HomeNet") == 0);
    assert(memcmp(got, mac, 6) == 0);

    // Wildcard probe (SSID len 0) → empty string, still true.
    len = mk_probe(buf, mac, "");
    assert(wlan_probe_extract(buf, len, ssid, got));
    assert(ssid[0] == '\0');

    // Not a probe-request (beacon FC 0x80) → false.
    buf[0] = 0x80;
    assert(!wlan_probe_extract(buf, len, ssid, got));

    // Too short → false.
    assert(!wlan_probe_extract(buf, 10, ssid, got));

    // SSID set: dedup + bounded + skips empty.
    WlanSsidSet set;
    wlan_ssid_set_reset(&set);
    assert(wlan_ssid_set_insert(&set, "A"));
    assert(!wlan_ssid_set_insert(&set, "A"));   // dup
    assert(!wlan_ssid_set_insert(&set, ""));     // empty skipped
    assert(wlan_ssid_set_insert(&set, "B"));
    assert(set.count == 2);
    char name[8];
    for (int i = 0; i < WLAN_SSID_SET_CAP + 5; i++) {
        snprintf(name, sizeof(name), "s%d", i);
        wlan_ssid_set_insert(&set, name);
    }
    assert(set.count == WLAN_SSID_SET_CAP);      // never overflows

    printf("test_wlan_probe_parse OK\n");
    return 0;
}
