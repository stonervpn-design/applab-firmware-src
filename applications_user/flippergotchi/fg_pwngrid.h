// pwngrid social: parse fellow-pwnagotchi beacons (name/pwnd_tot/face), maintain a
// peer table, build our own advertisement, and persist a lifetime friends registry.
#pragma once

#include "fg_types.h"
#include <storage/storage.h>

extern const uint8_t FG_PWNGRID_MAC[6]; // de:ad:be:ef:de:ad

// Parse a pwngrid beacon's JSON (scanned from offset >=38, bounds-checked).
// Fills out->name/pwnd/face; returns false if no name field found.
bool fg_pwngrid_parse(const uint8_t* frame, int len, FgPeer* out);

// Dedup by name into the table (refresh rssi/last_seen); returns index or -1 if full.
int fg_pwngrid_note(FgPeerTable* t, const FgPeer* peer, int8_t rssi, uint32_t now);

// Build an 802.11 beacon from FG_PWNGRID_MAC carrying our identity JSON. Returns length.
uint16_t fg_pwngrid_build_adv(
    uint8_t* out,
    const char* name,
    const char* face,
    uint32_t pwnd_tot,
    uint32_t pwnd_run,
    uint32_t uptime);

// Lifetime friends registry (names + times-seen), persisted across reboots.
void fg_friends_load(Storage* s, uint32_t* out_total_unique);
void fg_friends_note(Storage* s, const char* name); // append/increment (best-effort)
