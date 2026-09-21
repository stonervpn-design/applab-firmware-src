// Per-AP dedup table: find/insert/evict by BSSID, record SSID/RSSI/flags/channel.
// Pure logic (no ESP-IDF deps) — mirrored by tests/fg_oracle.py.
#pragma once

#include "fg_types.h"

// Index of bssid in the table, or -1.
int fg_net_find(const FgNetTable* t, const uint8_t bssid[6]);

// Find-or-insert by bssid, refreshing channel/rssi/last_seen. When full, evict the
// least-recently-seen NOT-pwned entry; if all are pwned, returns -1 (drop).
int fg_net_observe(FgNetTable* t, const uint8_t bssid[6], uint8_t channel, int8_t rssi, uint32_t now);

void fg_net_set_ssid(FgNetTable* t, int idx, const char* ssid);

// Sets flag; returns true only if it was newly set (transition 0->1).
bool fg_net_add_flag(FgNetTable* t, int idx, uint8_t flag);

// HAS_PMKID or HAS_M2 => this net counts as pwned (and is excluded from attack).
bool fg_net_is_pwned(const FgNetTable* t, int idx);

uint16_t fg_net_pwned_count(const FgNetTable* t);
