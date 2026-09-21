#include "fg_net_table.h"
#include <string.h>
#include <freertos/FreeRTOS.h>

#ifndef __ATOMIC_RELEASE
#define fg_release_fence() __sync_synchronize()
#else
#define fg_release_fence() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif

// fg_net_observe/set_ssid/add_flag are called from BOTH the WiFi promiscuous
// callback (beacon path) and the worker task (EAPOL path). Serialize the table
// MUTATIONS with a short spinlock so two concurrent inserts can't interleave and
// corrupt an entry or lose a network. Read-only scans (find/is_pwned/pwned_count,
// used from the GUI draw) stay lock-free — a torn single-field read is cosmetic.
static portMUX_TYPE s_net_mux = portMUX_INITIALIZER_UNLOCKED;

int fg_net_find(const FgNetTable* t, const uint8_t bssid[6]) {
    uint16_t n = t->count;
    for(uint16_t i = 0; i < n; i++) {
        if(memcmp(t->nets[i].bssid, bssid, 6) == 0) return (int)i;
    }
    return -1;
}

bool fg_net_is_pwned(const FgNetTable* t, int idx) {
    if(idx < 0 || idx >= (int)t->count) return false;
    return (t->nets[idx].flags & (FG_NET_HAS_PMKID | FG_NET_HAS_M2)) != 0;
}

uint16_t fg_net_pwned_count(const FgNetTable* t) {
    uint16_t c = 0, n = t->count;
    for(uint16_t i = 0; i < n; i++)
        if(t->nets[i].flags & (FG_NET_HAS_PMKID | FG_NET_HAS_M2)) c++;
    return c;
}

int fg_net_observe(
    FgNetTable* t,
    const uint8_t bssid[6],
    uint8_t channel,
    int8_t rssi,
    uint32_t now) {
    portENTER_CRITICAL(&s_net_mux);
    int result;
    int idx = fg_net_find(t, bssid);
    if(idx >= 0) {
        t->nets[idx].channel = channel;
        t->nets[idx].rssi = rssi;
        t->nets[idx].last_seen = now;
        result = idx;
    } else if(t->count < FG_MAX_NETS) {
        idx = t->count;
        FgNet* e = &t->nets[idx];
        memset(e, 0, sizeof(*e));
        memcpy(e->bssid, bssid, 6);
        e->channel = channel;
        e->rssi = rssi;
        e->last_seen = now;
        fg_release_fence();
        t->count = idx + 1;
        result = idx;
    } else {
        // Full: evict the least-recently-seen NOT-pwned entry.
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for(uint16_t i = 0; i < FG_MAX_NETS; i++) {
            if(t->nets[i].flags & (FG_NET_HAS_PMKID | FG_NET_HAS_M2)) continue;
            if(t->nets[i].last_seen <= oldest) {
                oldest = t->nets[i].last_seen;
                victim = (int)i;
            }
        }
        if(victim < 0) {
            result = -1; // every slot pwned — keep them, drop this obs.
        } else {
            FgNet* e = &t->nets[victim];
            memset(e, 0, sizeof(*e));
            memcpy(e->bssid, bssid, 6);
            e->channel = channel;
            e->rssi = rssi;
            e->last_seen = now;
            result = victim;
        }
    }
    portEXIT_CRITICAL(&s_net_mux);
    return result;
}

void fg_net_set_ssid(FgNetTable* t, int idx, const char* ssid) {
    portENTER_CRITICAL(&s_net_mux);
    // don't clobber a known SSID with empty
    if(idx >= 0 && idx < (int)t->count && ssid && !(t->nets[idx].ssid[0] && !ssid[0])) {
        strncpy(t->nets[idx].ssid, ssid, sizeof(t->nets[idx].ssid) - 1);
        t->nets[idx].ssid[sizeof(t->nets[idx].ssid) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_net_mux);
}

bool fg_net_add_flag(FgNetTable* t, int idx, uint8_t flag) {
    portENTER_CRITICAL(&s_net_mux);
    bool ret = false;
    if(idx >= 0 && idx < (int)t->count) {
        bool was = (t->nets[idx].flags & flag) != 0;
        t->nets[idx].flags |= flag;
        ret = !was;
    }
    portEXIT_CRITICAL(&s_net_mux);
    return ret;
}
