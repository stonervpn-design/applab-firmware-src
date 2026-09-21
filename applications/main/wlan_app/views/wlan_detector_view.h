#pragma once
#include <gui/view.h>
#include <stdint.h>

#define DET_ALERT_ROWS 4

typedef struct {
    uint32_t deauth_per_sec;
    uint32_t pwnagotchi;
    uint32_t evil_twin;
    char alerts[DET_ALERT_ROWS][32];
    uint8_t alert_head;
    uint8_t alert_count;
} WlanDetectorViewModel;

View* wlan_detector_view_alloc(void);
void wlan_detector_view_free(View* v);
void wlan_detector_view_push_alert(View* v, const char* line);
void wlan_detector_view_set_counters(View* v, uint32_t d, uint32_t p, uint32_t e);
