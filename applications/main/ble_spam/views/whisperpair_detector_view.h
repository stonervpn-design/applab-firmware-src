#pragma once

#include <gui/view.h>
#include "../ble_compat_defs.h"
#include "../whisperpair_fp.h"

#define WP_DETECTOR_MAX_DEVICES     24
#define WP_DETECTOR_ITEMS_ON_SCREEN 4

typedef enum {
    WpStatusUnknown = 0,
    WpStatusProbing,
    WpStatusNoSurface,    // connected, but no 0xFE2C service / no KBP characteristic
    WpStatusConnectFail,
    WpStatusSurface,      // 0xFE2C + Key-Based Pairing characteristic present
    WpStatusLikelyVuln,   // WpStatusSurface AND name matches known-vulnerable list
} WpStatus;

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    char name[24];
    FpAdvKind adv_kind;
    uint32_t model_id;
    bool model_id_valid;
    WpStatus status;
} WpDevice;

typedef struct {
    WpDevice devices[WP_DETECTOR_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    bool probing;
} WpDetectorModel;

View* whisperpair_detector_view_alloc(void);
void whisperpair_detector_view_free(View* view);
