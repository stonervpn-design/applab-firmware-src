#pragma once
#include "wlan_probe_parse.h"

typedef enum {
    WlanAlertNone = 0,
    WlanAlertDeauth,
    WlanAlertPwnagotchi,
} WlanAlertType;

WlanAlertType wlan_attack_classify(const uint8_t* frame, uint16_t len, uint8_t out_src[6]);

#define WLAN_ET_CAP 16
typedef struct { char ssid[WLAN_PROBE_SSID_MAX]; uint8_t bssid[6]; bool used; } WlanEtEntry;
typedef struct { WlanEtEntry e[WLAN_ET_CAP]; } WlanEvilTwinTracker;
void wlan_evil_twin_reset(WlanEvilTwinTracker* t);
bool wlan_evil_twin_observe(WlanEvilTwinTracker* t, const char* ssid, const uint8_t bssid[6]);
