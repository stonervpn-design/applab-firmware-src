#include "wlan_probe_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void probe_draw(Canvas* canvas, void* model) {
    WlanProbeViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Probe Sniffer");
    canvas_set_font(canvas, FontSecondary);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "clients:%u ssids:%u",
             (unsigned)m->unique_clients, (unsigned)m->unique_ssids);
    canvas_draw_str(canvas, 2, 21, hdr);
    canvas_draw_line(canvas, 0, 24, 127, 24);
    int y = 33;
    for(uint8_t i = 0; i < m->line_count && i < PROBE_VIEW_ROWS; i++) {
        canvas_draw_str(canvas, 2, y, m->lines[i]);
        y += 9;
    }
}

static bool probe_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false;
}

View* wlan_probe_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanProbeViewModel));
    view_set_draw_callback(v, probe_draw);
    view_set_input_callback(v, probe_input);
    return v;
}

void wlan_probe_view_free(View* v) { view_free(v); }

void wlan_probe_view_update(View* v, const char lines[][32], uint8_t n,
                            uint16_t clients, uint16_t ssids) {
    with_view_model(
        v, WlanProbeViewModel * m,
        {
            m->line_count = n > PROBE_VIEW_ROWS ? PROBE_VIEW_ROWS : n;
            for(uint8_t i = 0; i < m->line_count; i++) {
                strncpy(m->lines[i], lines[i], 31);
                m->lines[i][31] = '\0';
            }
            m->unique_clients = clients;
            m->unique_ssids = ssids;
        },
        true);
}
