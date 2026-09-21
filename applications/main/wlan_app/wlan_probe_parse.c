#include "wlan_probe_parse.h"

bool wlan_probe_ssid_ie(const uint8_t* frame, uint16_t len, uint16_t tag_offset,
                        char out_ssid[WLAN_PROBE_SSID_MAX]) {
    out_ssid[0] = '\0';
    if(!frame || len < tag_offset) return false;
    uint16_t i = tag_offset;
    while(i + 2 <= len) {
        uint8_t tag = frame[i];
        uint8_t tlen = frame[i + 1];
        if(i + 2 + tlen > len) return false; // malformed IE
        if(tag == 0x00) { // SSID IE
            uint8_t n = tlen > 32 ? 32 : tlen;
            memcpy(out_ssid, &frame[i + 2], n);
            out_ssid[n] = '\0';
            return true;
        }
        i += 2 + tlen;
    }
    return false;
}

bool wlan_probe_extract(const uint8_t* frame, uint16_t len,
                        char out_ssid[WLAN_PROBE_SSID_MAX], uint8_t out_mac[6]) {
    out_ssid[0] = '\0';
    if(!frame || len < 24) return false;
    // Frame control byte0: version(2) | type(2)<<2 | subtype(4)<<4.
    // Probe-request: type=00 (mgmt), subtype=0100 → 0x40.
    if((frame[0] & 0x0C) != 0x00) return false;  // must be mgmt
    if((frame[0] & 0xF0) != 0x40) return false;  // must be subtype 4
    memcpy(out_mac, &frame[10], 6);              // addr2 = transmitter
    // Tagged params begin at offset 24 for a probe-request. SSID absent = wildcard.
    wlan_probe_ssid_ie(frame, len, 24, out_ssid); // leaves "" if none
    return true;
}

void wlan_ssid_set_reset(WlanSsidSet* s) {
    s->count = 0;
}

bool wlan_ssid_set_insert(WlanSsidSet* s, const char* ssid) {
    if(!ssid || ssid[0] == '\0') return false;
    for(uint8_t i = 0; i < s->count; i++) {
        if(strncmp(s->ssids[i], ssid, WLAN_PROBE_SSID_MAX - 1) == 0) return false;
    }
    if(s->count >= WLAN_SSID_SET_CAP) return false;
    strncpy(s->ssids[s->count], ssid, WLAN_PROBE_SSID_MAX - 1);
    s->ssids[s->count][WLAN_PROBE_SSID_MAX - 1] = '\0';
    s->count++;
    return true;
}
