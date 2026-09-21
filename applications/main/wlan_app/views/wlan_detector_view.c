#include "wlan_detector_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void detector_draw(Canvas* canvas, void* model) {
    WlanDetectorViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Attack Detector");
    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "DA/s:%lu Pwn:%lu ET:%lu",
             (unsigned long)m->deauth_per_sec,
             (unsigned long)m->pwnagotchi,
             (unsigned long)m->evil_twin);
    canvas_draw_str(canvas, 2, 22, line);
    canvas_draw_line(canvas, 0, 25, 127, 25);

    // Alert ring, newest at bottom.
    int y = 34;
    uint8_t n = m->alert_count;
    for(uint8_t i = 0; i < n; i++) {
        // oldest-first
        uint8_t idx = (uint8_t)((m->alert_head + DET_ALERT_ROWS - n + i) % DET_ALERT_ROWS);
        canvas_draw_str(canvas, 2, y, m->alerts[idx]);
        y += 9;
        if(y > 63) break;
    }
}

static bool detector_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false; // scene-manager back handler navigates; scene runs on tick
}

View* wlan_detector_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanDetectorViewModel));
    view_set_draw_callback(v, detector_draw);
    view_set_input_callback(v, detector_input);
    return v;
}

void wlan_detector_view_free(View* v) { view_free(v); }

void wlan_detector_view_push_alert(View* v, const char* line) {
    with_view_model(
        v, WlanDetectorViewModel * m,
        {
            strncpy(m->alerts[m->alert_head], line, sizeof(m->alerts[0]) - 1);
            m->alerts[m->alert_head][sizeof(m->alerts[0]) - 1] = '\0';
            m->alert_head = (uint8_t)((m->alert_head + 1) % DET_ALERT_ROWS);
            if(m->alert_count < DET_ALERT_ROWS) m->alert_count++;
        },
        true);
}

void wlan_detector_view_set_counters(View* v, uint32_t d, uint32_t p, uint32_t e) {
    with_view_model(
        v, WlanDetectorViewModel * m,
        { m->deauth_per_sec = d; m->pwnagotchi = p; m->evil_twin = e; }, true);
}
