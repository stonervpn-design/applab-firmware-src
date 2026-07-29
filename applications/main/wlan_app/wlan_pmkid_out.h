#pragma once

#include <stdint.h>

typedef struct Storage Storage;

/* Append a captured PMKID to /ext/wifi/pmkid.22000 in hashcat -m 22000 format:
 * WPA*01*<pmkid>*<ap_mac>*<sta_mac>*<essid_hex>*** — clientless, crackable
 * offline. No-op if storage is NULL. */
void wlan_pmkid_save(
    Storage* storage,
    const uint8_t pmkid[16],
    const uint8_t ap[6],
    const uint8_t sta[6],
    const char* ssid);
