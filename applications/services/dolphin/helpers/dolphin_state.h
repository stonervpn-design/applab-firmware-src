#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dolphin_deed.h"
#include "dolphin_level.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DolphinFlagHappyMode = 1,
} DolphinFlags;

typedef struct DolphinState DolphinState;
typedef struct {
    uint8_t icounter_daily_limit[DolphinAppMAX];
    uint8_t butthurt_daily_limit;

    uint32_t flags;
    uint32_t icounter;
    int32_t butthurt;
    uint64_t timestamp;
} DolphinStoreData;

struct DolphinState {
    DolphinStoreData data;
    bool dirty;
};

DolphinState* dolphin_state_alloc(void);

void dolphin_state_free(DolphinState* dolphin_state);

void dolphin_state_save(DolphinState* dolphin_state);

void dolphin_state_load(DolphinState* dolphin_state);

void dolphin_state_clear_limits(DolphinState* dolphin_state);

uint64_t dolphin_state_timestamp(void);

void dolphin_state_on_deed(DolphinState* dolphin_state, DolphinDeed deed);

void dolphin_state_butthurted(DolphinState* dolphin_state);

// dolphin_state_xp_to_levelup / dolphin_state_xp_above_last_levelup /
// dolphin_state_is_levelup / dolphin_get_level are declared in dolphin_level.h
// (included above) and implemented in dolphin_level.c.

void dolphin_state_increase_level(DolphinState* dolphin_state);

#ifdef __cplusplus
}
#endif
