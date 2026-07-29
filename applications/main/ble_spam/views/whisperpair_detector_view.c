#include "whisperpair_detector_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static char wp_status_glyph(WpStatus s) {
    switch(s) {
    case WpStatusLikelyVuln:  return 'V';
    case WpStatusSurface:     return 'S';
    case WpStatusProbing:     return '.';
    case WpStatusNoSurface:   return '-';
    case WpStatusConnectFail: return 'x';
    case WpStatusUnknown:
    default:                  return '?';
    }
}

static void whisperpair_detector_draw_callback(Canvas* canvas, void* _model) {
    WpDetectorModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char title[32];
    snprintf(title, sizeof(title), "WhisperPair (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->count == 0) {
        canvas_draw_str_aligned(
            canvas, 64, 36, AlignCenter, AlignCenter, "Scanning FE2C...");
    }

    for(int i = 0;
        i < WP_DETECTOR_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count;
        i++) {
        uint16_t idx = model->window_offset + i;
        WpDevice* d = &model->devices[idx];

        const char* label = d->name[0] ? d->name : "(unnamed)";
        char advc = d->adv_kind == FpAdvDiscoverable ?
                        'd' :
                        (d->adv_kind == FpAdvHasAccounts ? 'a' : ' ');

        char line[40];
        snprintf(
            line,
            sizeof(line),
            "%c %-9.9s %c %d",
            wp_status_glyph(d->status),
            label,
            advc,
            d->rssi);

        uint8_t y = 22 + i * 11;
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->probing) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Probing...");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Probe");
    }
}

static bool whisperpair_detector_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp ||
           event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* whisperpair_detector_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WpDetectorModel));
    view_set_draw_callback(view, whisperpair_detector_draw_callback);
    view_set_input_callback(view, whisperpair_detector_input_callback);
    return view;
}

void whisperpair_detector_view_free(View* view) {
    view_free(view);
}
