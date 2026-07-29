#include "wlan_pmkid_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>

static const char* pmkid_state_str(PmkidState s) {
    switch(s) {
    case PmkidStateAssociating: return "Associating...";
    case PmkidStateWaiting:     return "Waiting for M1...";
    case PmkidStateFallback:    return "Deauth fallback...";
    case PmkidStateCaptured:    return "PMKID CAPTURED!";
    default:                    return "?";
    }
}

static void wlan_pmkid_view_draw(Canvas* canvas, void* model) {
    WlanPmkidViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "PMKID Attack");

    canvas_set_font(canvas, FontSecondary);
    char line[64];
    snprintf(line, sizeof(line), "SSID: %.20s", m->ssid[0] ? m->ssid : "(hidden)");
    canvas_draw_str(canvas, 2, 24, line);
    snprintf(line, sizeof(line), "CH %u  tries %lu", (unsigned)m->channel, (unsigned long)m->attempts);
    canvas_draw_str(canvas, 2, 36, line);
    canvas_draw_str(canvas, 2, 48, pmkid_state_str(m->state));

    if(m->have_pmkid) {
        snprintf(line, sizeof(line), "%.16s..", m->pmkid_hex);
        canvas_draw_str(canvas, 2, 60, line);
    } else {
        elements_button_left(canvas, "Back");
    }
}

static bool wlan_pmkid_view_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false; /* scene handles navigation via the dispatcher back-handler */
}

View* wlan_pmkid_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanPmkidViewModel));
    view_set_draw_callback(view, wlan_pmkid_view_draw);
    view_set_input_callback(view, wlan_pmkid_view_input);
    return view;
}

void wlan_pmkid_view_free(View* view) {
    view_free(view);
}
