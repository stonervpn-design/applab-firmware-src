#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PmkidStateAssociating,
    PmkidStateWaiting,
    PmkidStateFallback,
    PmkidStateCaptured,
} PmkidState;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    PmkidState state;
    uint32_t attempts;
    bool have_pmkid;
    char pmkid_hex[33]; /* 16 bytes -> 32 hex chars + NUL */
} WlanPmkidViewModel;

View* wlan_pmkid_view_alloc(void);
void wlan_pmkid_view_free(View* view);
