#pragma once
#include "wlan_probe_parse.h"

#define WLAN_PT_CAP 24
#define WLAN_PT_SSID_PER 4

typedef struct {
    uint8_t mac[6];
    char ssids[WLAN_PT_SSID_PER][WLAN_PROBE_SSID_MAX];
    uint8_t ssid_count;
    uint32_t seq;
    bool used;
} WlanPtEntry;

typedef struct {
    WlanPtEntry entries[WLAN_PT_CAP];
    uint32_t clock;
    uint16_t unique_clients;
    WlanSsidSet all_ssids;
} WlanProbeTable;

void wlan_probe_table_reset(WlanProbeTable* t);
void wlan_probe_table_add(WlanProbeTable* t, const uint8_t mac[6], const char* ssid);
uint16_t wlan_probe_table_client_count(const WlanProbeTable* t);
uint16_t wlan_probe_table_ssid_count(const WlanProbeTable* t);
