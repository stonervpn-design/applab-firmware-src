// Name, XP/level curve, streaks, achievements + persistence (state file with legacy
// .score migration). Pure math is mirrored by tests/fg_oracle.py.
#pragma once

#include "fg_types.h"
#include <storage/storage.h>

uint32_t fg_xp_for_level(uint32_t level); // triangular: 10*L*(L+1)/2
uint32_t fg_level_of(uint32_t xp);
void fg_progress_add_xp(FgProgress* p, uint32_t dxp);
void fg_progress_default_name(FgProgress* p, uint32_t seed);

// Sets any newly-earned achievement bits; returns the newly-unlocked mask (0 if none).
uint32_t fg_award_check(
    FgProgress* p,
    uint32_t hs,
    uint32_t pmkid,
    uint32_t friends,
    uint32_t full_hs,
    uint32_t session_secs,
    uint8_t channels_hit);

const char* fg_ach_name(uint32_t bit); // human label for one achievement bit

bool fg_progress_load(Storage* s, FgProgress* p); // false if fresh (name still empty)
void fg_progress_save(Storage* s, const FgProgress* p);
