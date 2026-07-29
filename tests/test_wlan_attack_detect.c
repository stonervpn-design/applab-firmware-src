#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include "../applications/main/wlan_app/wlan_attack_detect.c"
#include <assert.h>
#include <stdio.h>

// 24-byte mgmt header with given FC subtype byte + addr2.
static uint16_t mk_mgmt(uint8_t* b, uint8_t fc0, const uint8_t src[6]) {
    memset(b, 0, 24);
    b[0] = fc0;
    for (int i = 0; i < 6; i++) b[10 + i] = src[i]; // addr2
    return 24;
}

// beacon (fc0=0x80): 24 hdr + 12 fixed (ts/interval/cap) + SSID IE @36.
static uint16_t mk_beacon(uint8_t* b, const uint8_t bssid[6], const char* ssid) {
    memset(b, 0, 36);
    b[0] = 0x80;
    for (int i = 0; i < 6; i++) { b[10+i] = bssid[i]; b[16+i] = bssid[i]; }
    uint16_t n = 36;
    uint8_t sl = (uint8_t)strlen(ssid);
    b[n++] = 0x00; b[n++] = sl;
    for (uint8_t i = 0; i < sl; i++) b[n++] = (uint8_t)ssid[i];
    return n;
}

int main(void) {
    uint8_t buf[128], src[6] = {1,2,3,4,5,6}, got[6];

    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0xC0, src), got) == WlanAlertDeauth);
    assert(memcmp(got, src, 6) == 0);
    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0xA0, src), NULL) == WlanAlertDeauth); // disassoc
    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0x40, src), NULL) == WlanAlertNone);   // probe-req

    uint8_t bss[6] = {0xDE,0xAD,0,0,0,1};
    assert(wlan_attack_classify(buf, mk_beacon(buf, bss, "{\"name\":\"pwn\""), NULL)
           == WlanAlertPwnagotchi);
    assert(wlan_attack_classify(buf, mk_beacon(buf, bss, "NormalNet"), NULL)
           == WlanAlertNone);

    // Evil-twin tracker
    WlanEvilTwinTracker et; wlan_evil_twin_reset(&et);
    uint8_t b1[6] = {1,1,1,1,1,1}, b2[6] = {2,2,2,2,2,2};
    assert(!wlan_evil_twin_observe(&et, "Cafe", b1)); // first sighting
    assert(!wlan_evil_twin_observe(&et, "Cafe", b1)); // same bssid, fine
    assert(wlan_evil_twin_observe(&et, "Cafe", b2));   // different bssid → twin!
    assert(!wlan_evil_twin_observe(&et, "Other", b1)); // unrelated

    printf("test_wlan_attack_detect OK\n");
    return 0;
}
