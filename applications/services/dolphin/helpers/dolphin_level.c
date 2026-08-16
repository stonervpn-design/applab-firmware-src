#include "dolphin_level.h"

#include <stddef.h>

// Level boundaries: dolphin_level_thresholds[i] = round(300 * (i+1)^1.574),
// for i = 0..28 (29 values, computed offline -- see task-1-report.md for the
// generating script; C has no compile-time pow()).
//
// Semantics:
//   level 1            : icounter in [0, thresholds[0]]
//   level (i+2)         : icounter in (thresholds[i], thresholds[i+1]]
//   level DOLPHIN_LEVEL_COUNT (30): icounter in (thresholds[28], inf)
// i.e. crossing thresholds[i] (going from thresholds[i] to thresholds[i]+1)
// is the level-up from level (i+1) to level (i+2).
static const uint32_t dolphin_level_thresholds[DOLPHIN_LEVEL_COUNT - 1] = {
    300,   893,   1691,  2659,  3778,  5034,  6417,  7917,  9530,  11249,
    13070, 14988, 17001, 19104, 21296, 23573, 25933, 28374, 30894, 33492,
    36165, 38913, 41733, 44625, 47586, 50616, 53714, 56878, 60108,
};

uint8_t dolphin_get_level(uint32_t icounter) {
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT - 1; i++) {
        if(icounter <= dolphin_level_thresholds[i]) {
            return (uint8_t)(i + 1);
        }
    }
    return DOLPHIN_LEVEL_COUNT;
}

bool dolphin_state_is_levelup(uint32_t icounter) {
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT - 1; i++) {
        if(icounter == dolphin_level_thresholds[i]) {
            return true;
        }
    }
    return false;
}

uint32_t dolphin_state_xp_above_last_levelup(uint32_t icounter) {
    uint32_t lower_bound = 0;
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT - 1; i++) {
        if(icounter <= dolphin_level_thresholds[i]) {
            break;
        }
        lower_bound = dolphin_level_thresholds[i] + 1;
    }
    return icounter - lower_bound;
}

uint32_t dolphin_state_xp_to_levelup(uint32_t icounter) {
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT - 1; i++) {
        if(icounter <= dolphin_level_thresholds[i]) {
            return dolphin_level_thresholds[i] - icounter;
        }
    }
    return 0; // already at level DOLPHIN_LEVEL_COUNT, no further level-up
}
