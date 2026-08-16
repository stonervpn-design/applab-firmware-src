#pragma once

// Pure integer-math dolphin level system: threshold table + the 4 level
// functions. Deliberately free of furi/storage includes so it host-compiles
// standalone (see tools/dolphin_level_host_test/).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOLPHIN_LEVEL_COUNT 30

// icounter -> level, 1..DOLPHIN_LEVEL_COUNT (clamped at the top).
uint8_t dolphin_get_level(uint32_t icounter);

// True iff icounter sits exactly on a level-up boundary.
bool dolphin_state_is_levelup(uint32_t icounter);

// icounter minus the lower bound of the current level.
uint32_t dolphin_state_xp_above_last_levelup(uint32_t icounter);

// Boundary of the current level minus icounter; 0 once at the level cap.
uint32_t dolphin_state_xp_to_levelup(uint32_t icounter);

#ifdef __cplusplus
}
#endif
