#include "nrf24_jam_view.h"

#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

/* Mode metadata — single source of truth (worker in scene_jam.c uses these). */
static const char* const MODE_NAMES[NRF24_JAM_MODE_COUNT] = {
    "WiFi",
    "BT Classic",
    "BLE Adv",
    "BLE Data",
    "Zigbee",
    "RC",
    "USB",
    "Video",
    "Full Band",
#ifndef PUBLIC_BUILD
    "BLE Reactive",
#endif
};

const char* nrf24_jam_mode_name(int mode) {
    return MODE_NAMES[(mode >= 0 && mode < NRF24_JAM_MODE_COUNT) ? mode : 0];
}

const char* nrf24_jam_pa_name(uint8_t pa_level) {
    static const char* const N[4] = {"Min", "Low", "High", "Max"};
    return N[pa_level & 0x03];
}

bool nrf24_jam_mode_is_flood(int mode) {
    return mode == 0 || mode == 4; /* WiFi (OFDM), Zigbee (DSSS) -> noise flood */
}

bool nrf24_jam_mode_is_reactive(int mode) {
#ifndef PUBLIC_BUILD
    return mode == 9; /* BLE Reactive: RPD-triggered CW, not flood/plain-CW */
#else
    (void)mode;
    return false;
#endif
}

/* Scrolling list nav — selected row shows ">" (or "*" when jamming) + strategy;
 * driven by Down/OK only so it's identical on the Cardputer and 2-button StickS3. */
static void nrf24_jam_draw_callback(Canvas* canvas, void* _model) {
    Nrf24JamModel* model = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "nRF24 Jammer");
    canvas_set_font(canvas, FontSecondary);

    /* Current PA level, top-right, always visible. */
    char pa[16];
    snprintf(pa, sizeof(pa), "PA:%s", nrf24_jam_pa_name(model->pa_level));
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, pa);

    if(!model->hardware_ok) {
        canvas_draw_str(canvas, 2, 34, "NO nRF24 MODULE!");
        canvas_draw_str(canvas, 2, 63, "Back: exit");
        return;
    }

    const int visible = 4;
    int top = model->mode - 1;
    if(top > NRF24_JAM_MODE_COUNT - visible) top = NRF24_JAM_MODE_COUNT - visible;
    if(top < 0) top = 0;

    char line[40];
    for(int r = 0; r < visible && (top + r) < NRF24_JAM_MODE_COUNT; r++) {
        int idx = top + r;
        int y = 22 + r * 11;
        bool sel = (idx == model->mode);
        bool on = (model->running && idx == model->mode);
        if(sel) {
            snprintf(
                line, sizeof(line), "%c %s (%s)", on ? '*' : '>', nrf24_jam_mode_name(idx),
                nrf24_jam_mode_is_flood(idx) ? "noise" : "CW");
        } else {
            snprintf(line, sizeof(line), "  %s", nrf24_jam_mode_name(idx));
        }
        canvas_draw_str(canvas, 2, y, line);
    }

    canvas_draw_str(
        canvas, 2, 63, model->running ? "*JAM OK:stop  L/R:PA" : "Dn:mode OK:jam L/R:PA");
}

static bool nrf24_jam_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyOk:
        view_dispatcher_send_custom_event(vd, Nrf24JamEventToggle);
        return true;
    case InputKeyDown:
        view_dispatcher_send_custom_event(vd, Nrf24JamEventNext);
        return true;
    case InputKeyUp:
        view_dispatcher_send_custom_event(vd, Nrf24JamEventPrev);
        return true;
    case InputKeyLeft:
        view_dispatcher_send_custom_event(vd, Nrf24JamEventPaDown);
        return true;
    case InputKeyRight:
        view_dispatcher_send_custom_event(vd, Nrf24JamEventPaUp);
        return true;
    default:
        return false; /* Back falls through to view_dispatcher nav (pops scene) */
    }
}

View* nrf24_jam_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(Nrf24JamModel));
    view_set_draw_callback(view, nrf24_jam_draw_callback);
    view_set_input_callback(view, nrf24_jam_input_callback);
    return view;
}

void nrf24_jam_view_free(View* view) {
    view_free(view);
}
