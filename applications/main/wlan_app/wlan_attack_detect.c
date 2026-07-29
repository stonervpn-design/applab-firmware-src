#include "wlan_attack_detect.h"

WlanAlertType wlan_attack_classify(const uint8_t* frame, uint16_t len, uint8_t out_src[6]) {
    if(!frame || len < 24) return WlanAlertNone;
    if((frame[0] & 0x0C) != 0x00) return WlanAlertNone; // mgmt only
    uint8_t subtype = frame[0] & 0xF0;
    if(out_src) memcpy(out_src, &frame[10], 6);
    if(subtype == 0xC0 || subtype == 0xA0) return WlanAlertDeauth; // deauth / disassoc
    if(subtype == 0x80) { // beacon → pwnagotchi heuristic
        char ssid[WLAN_PROBE_SSID_MAX];
        if(wlan_probe_ssid_ie(frame, len, 36, ssid)) {
            if(ssid[0] == '{' && ssid[1] == '"') return WlanAlertPwnagotchi;
        }
    }
    if(out_src) memset(out_src, 0, 6);
    return WlanAlertNone;
}

void wlan_evil_twin_reset(WlanEvilTwinTracker* t) {
    memset(t, 0, sizeof(*t));
}

bool wlan_evil_twin_observe(WlanEvilTwinTracker* t, const char* ssid, const uint8_t bssid[6]) {
    if(!ssid || ssid[0] == '\0') return false;
    WlanEtEntry* free_slot = NULL;
    for(uint8_t i = 0; i < WLAN_ET_CAP; i++) {
        WlanEtEntry* e = &t->e[i];
        if(!e->used) { if(!free_slot) free_slot = e; continue; }
        if(strncmp(e->ssid, ssid, WLAN_PROBE_SSID_MAX - 1) == 0) {
            return memcmp(e->bssid, bssid, 6) != 0; // same ssid, different bssid = twin
        }
    }
    if(free_slot) { // record first sighting (bounded; extra SSIDs ignored once full)
        strncpy(free_slot->ssid, ssid, WLAN_PROBE_SSID_MAX - 1);
        free_slot->ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
        memcpy(free_slot->bssid, bssid, 6);
        free_slot->used = true;
    }
    return false;
}
