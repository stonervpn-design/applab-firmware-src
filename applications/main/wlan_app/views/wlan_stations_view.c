#include "wlan_stations_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void stations_draw(Canvas* canvas, void* model) {
    WlanStationsViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Station Scanner");
    canvas_set_font(canvas, FontSecondary);
    char hdr[32];
    snprintf(
        hdr, sizeof(hdr), "aps:%u clients:%u",
        (unsigned)m->ap_count, (unsigned)m->client_total);
    canvas_draw_str(canvas, 2, 21, hdr);
    canvas_draw_line(canvas, 0, 24, 127, 24);
    int y = 33;
    if(m->line_count == 0) {
        canvas_draw_str(canvas, 2, y, "listening...");
        return;
    }
    for(uint8_t i = 0; i < m->line_count && i < STATIONS_VIEW_ROWS; i++) {
        canvas_draw_str(canvas, 2, y, m->lines[i]);
        y += 9;
    }
}

static bool stations_input(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false;
}

View* wlan_stations_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanStationsViewModel));
    view_set_draw_callback(v, stations_draw);
    view_set_input_callback(v, stations_input);
    return v;
}

void wlan_stations_view_free(View* v) {
    view_free(v);
}

void wlan_stations_view_update(
    View* v,
    const char lines[][32],
    uint8_t n,
    uint16_t aps,
    uint16_t clients) {
    with_view_model(
        v,
        WlanStationsViewModel * m,
        {
            m->line_count = n > STATIONS_VIEW_ROWS ? STATIONS_VIEW_ROWS : n;
            for(uint8_t i = 0; i < m->line_count; i++) {
                strncpy(m->lines[i], lines[i], 31);
                m->lines[i][31] = '\0';
            }
            m->ap_count = aps;
            m->client_total = clients;
        },
        true);
}
