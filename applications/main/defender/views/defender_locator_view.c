#include "defender_locator_view.h"
#include "../defender_locator_map.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <stdio.h>

static const char* proximity_label(uint8_t bar_level, bool stale) {
    if(stale) return "NO SIGNAL";
    if(bar_level >= 9) return "RIGHT HERE";
    if(bar_level >= 7) return "HOT";
    if(bar_level >= 4) return "WARM";
    if(bar_level >= 1) return "COOL";
    return "COLD";
}

static void defender_locator_draw_callback(Canvas* canvas, void* _model) {
    DefenderLocatorModel* model = _model;
    canvas_clear(canvas);

    // Header: label text
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, model->label);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    // Calculate bar level for proximity label and bar graph
    uint8_t bar_level = defender_locator_bar_level(model->rssi, model->floor, model->ceil);

    // Proximity label (large, centered)
    canvas_set_font(canvas, FontPrimary);
    const char* prox = proximity_label(bar_level, model->stale);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, prox);

    // RSSI value
    canvas_set_font(canvas, FontSecondary);
    char rssi_text[16];
    if(model->stale) {
        snprintf(rssi_text, sizeof(rssi_text), "--- dBm");
    } else {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", model->rssi);
    }
    canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, rssi_text);

    // RSSI bargraph
    const uint8_t bar_x = 4;
    const uint8_t bar_y = 54;
    const uint8_t bar_w = 120;
    const uint8_t bar_h = 7;
    elements_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    if(!model->stale) {
        uint8_t fill_w = (uint8_t)((bar_level * (bar_w - 2)) / LOCATOR_BAR_MAX);
        if(fill_w > 0) {
            canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);
        }
    }
}

static bool defender_locator_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    // Back is handled by view_dispatcher's navigation callback. No other input.
    return false;
}

View* defender_locator_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(DefenderLocatorModel));
    view_set_draw_callback(view, defender_locator_draw_callback);
    view_set_input_callback(view, defender_locator_input_callback);
    return view;
}

void defender_locator_view_free(View* view) {
    view_free(view);
}
