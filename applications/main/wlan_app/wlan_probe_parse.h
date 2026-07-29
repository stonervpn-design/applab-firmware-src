#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define WLAN_PROBE_SSID_MAX 33

bool wlan_probe_extract(const uint8_t* frame, uint16_t len,
                        char out_ssid[WLAN_PROBE_SSID_MAX], uint8_t out_mac[6]);
bool wlan_probe_ssid_ie(const uint8_t* frame, uint16_t len, uint16_t tag_offset,
                        char out_ssid[WLAN_PROBE_SSID_MAX]);

#define WLAN_SSID_SET_CAP 32
typedef struct {
    char ssids[WLAN_SSID_SET_CAP][WLAN_PROBE_SSID_MAX];
    uint8_t count;
} WlanSsidSet;

void wlan_ssid_set_reset(WlanSsidSet* s);
bool wlan_ssid_set_insert(WlanSsidSet* s, const char* ssid);
