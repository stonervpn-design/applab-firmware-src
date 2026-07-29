#include "wlan_probe_table.h"

void wlan_probe_table_reset(WlanProbeTable* t) {
    memset(t, 0, sizeof(*t));
    wlan_ssid_set_reset(&t->all_ssids);
}

static WlanPtEntry* pt_find(WlanProbeTable* t, const uint8_t mac[6]) {
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(t->entries[i].used && memcmp(t->entries[i].mac, mac, 6) == 0) {
            return &t->entries[i];
        }
    }
    return NULL;
}

static WlanPtEntry* pt_alloc(WlanProbeTable* t, const uint8_t mac[6]) {
    // First free slot, else LRU (lowest seq).
    WlanPtEntry* victim = NULL;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(!t->entries[i].used) { victim = &t->entries[i]; break; }
        if(!victim || t->entries[i].seq < victim->seq) victim = &t->entries[i];
    }
    memset(victim, 0, sizeof(*victim));
    memcpy(victim->mac, mac, 6);
    victim->used = true;
    return victim;
}

static void pt_add_ssid(WlanPtEntry* e, const char* ssid) {
    if(!ssid || ssid[0] == '\0') return;
    for(uint8_t i = 0; i < e->ssid_count; i++) {
        if(strncmp(e->ssids[i], ssid, WLAN_PROBE_SSID_MAX - 1) == 0) return;
    }
    if(e->ssid_count < WLAN_PT_SSID_PER) {
        strncpy(e->ssids[e->ssid_count], ssid, WLAN_PROBE_SSID_MAX - 1);
        e->ssids[e->ssid_count][WLAN_PROBE_SSID_MAX - 1] = '\0';
        e->ssid_count++;
    } else {
        // shift out oldest, append newest
        for(uint8_t i = 1; i < WLAN_PT_SSID_PER; i++) {
            memcpy(e->ssids[i - 1], e->ssids[i], WLAN_PROBE_SSID_MAX);
        }
        strncpy(e->ssids[WLAN_PT_SSID_PER - 1], ssid, WLAN_PROBE_SSID_MAX - 1);
        e->ssids[WLAN_PT_SSID_PER - 1][WLAN_PROBE_SSID_MAX - 1] = '\0';
    }
}

void wlan_probe_table_add(WlanProbeTable* t, const uint8_t mac[6], const char* ssid) {
    WlanPtEntry* e = pt_find(t, mac);
    if(!e) {
        e = pt_alloc(t, mac);
        t->unique_clients++;
    }
    e->seq = ++t->clock;
    pt_add_ssid(e, ssid);
    wlan_ssid_set_insert(&t->all_ssids, ssid);
}

uint16_t wlan_probe_table_client_count(const WlanProbeTable* t) {
    uint16_t n = 0;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) if(t->entries[i].used) n++;
    return n;
}

uint16_t wlan_probe_table_ssid_count(const WlanProbeTable* t) {
    return t->all_ssids.count;
}
