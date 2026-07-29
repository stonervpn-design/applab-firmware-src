#include "defender_ratemeter_view.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <stdio.h>

static void defender_ratemeter_draw_callback(Canvas* canvas, void* _model) {
    DefenderRatemeterModel* model = _model;
    canvas_clear(canvas);

    /* Header: title with alert indicator */
    canvas_set_font(canvas, FontPrimary);
    if(model->alert) {
        /* Invert header to highlight alert */
        canvas_draw_box(canvas, 0, 0, 128, 14);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, 2, 10, model->title);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_str(canvas, 2, 10, model->title);
    }
    canvas_draw_line(canvas, 0, 12, 128, 12);

    /* Big rate display: "%lu/s" centered */
    canvas_set_font(canvas, FontPrimary);
    char rate_text[16];
    snprintf(rate_text, sizeof(rate_text), "%lu/s", (unsigned long)model->rate);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, rate_text);

    /* Top offender line: "AA:BB:CC:DD:EE:FF xNNN" */
    canvas_set_font(canvas, FontSecondary);
    char bssid_text[32];
    snprintf(bssid_text, sizeof(bssid_text), "%02X:%02X:%02X:%02X:%02X:%02X x%u",
             model->top_bssid[0], model->top_bssid[1], model->top_bssid[2],
             model->top_bssid[3], model->top_bssid[4], model->top_bssid[5],
             (unsigned)model->top_count);
    canvas_draw_str(canvas, 2, 50, bssid_text);

    /* RSSI display */
    char rssi_text[16];
    snprintf(rssi_text, sizeof(rssi_text), "%d dBm", model->top_rssi);
    canvas_draw_str(canvas, 2, 62, rssi_text);
}

static bool defender_ratemeter_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    /* OK will be handled by scene's custom event routing */
    return false;
}

View* defender_ratemeter_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(DefenderRatemeterModel));
    view_set_draw_callback(view, defender_ratemeter_draw_callback);
    view_set_input_callback(view, defender_ratemeter_input_callback);
    return view;
}

void defender_ratemeter_view_free(View* view) {
    view_free(view);
}
