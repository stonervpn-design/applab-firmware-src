#include "lan_attack_view_scan_status.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include <stdio.h>

static void lan_attack_scan_status_draw_callback(Canvas* canvas, void* _model) {
    LanAttackScanStatusModel* m = _model;
    canvas_clear(canvas);

    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, m->done ? "Recon Complete" : "LAN Recon");
    canvas_draw_line(canvas, 0, 12, 128, 12);

    // Progress bar
    const uint8_t bar_x = 4, bar_y = 16, bar_w = 120, bar_h = 8;
    elements_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    uint8_t pct = m->progress_pct > 100 ? 100 : m->progress_pct;
    uint8_t fill = (uint8_t)((pct * (bar_w - 2)) / 100);
    if(fill > 0) canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill, bar_h - 2);

    canvas_set_font(canvas, FontSecondary);
    char line[40];

    snprintf(line, sizeof(line), "Hosts: %u/%u", (unsigned)m->hosts_alive, (unsigned)m->hosts_total);
    canvas_draw_str(canvas, 2, 34, line);

    snprintf(line, sizeof(line), "Ports: %u", (unsigned)m->ports_found);
    canvas_draw_str(canvas, 2, 44, line);

    snprintf(line, sizeof(line), "Elapsed: %lus", (unsigned long)m->elapsed_sec);
    canvas_draw_str(canvas, 70, 44, line);

    if(m->done) {
        if(m->csv_name[0]) {
            snprintf(line, sizeof(line), "%s", m->csv_name);
            canvas_draw_str(canvas, 2, 54, line);
        }
        elements_button_center(canvas, "Back");
    } else {
        snprintf(line, sizeof(line), "%u%%", (unsigned)pct);
        canvas_draw_str(canvas, 2, 54, line);
        elements_button_left(canvas, "Cancel");
    }
}

static bool lan_attack_scan_status_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    UNUSED(event);
    // Back is handled by the view_dispatcher navigation callback (cancels scan).
    return false;
}

View* lan_attack_view_scan_status_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(LanAttackScanStatusModel));
    view_set_draw_callback(view, lan_attack_scan_status_draw_callback);
    view_set_input_callback(view, lan_attack_scan_status_input_callback);

    with_view_model(
        view,
        LanAttackScanStatusModel * m,
        {
            memset(m, 0, sizeof(*m));
            m->hosts_total = 254;
        },
        true);
    return view;
}

void lan_attack_view_scan_status_free(View* view) {
    view_free(view);
}

void lan_attack_view_scan_status_update(
    View* view,
    uint8_t progress_pct,
    uint16_t hosts_alive,
    uint16_t hosts_total,
    uint16_t ports_found,
    uint32_t elapsed_sec,
    bool done,
    const char* csv_name) {
    with_view_model(
        view,
        LanAttackScanStatusModel * m,
        {
            m->progress_pct = progress_pct;
            m->hosts_alive = hosts_alive;
            m->hosts_total = hosts_total;
            m->ports_found = ports_found;
            m->elapsed_sec = elapsed_sec;
            m->done = done;
            if(csv_name) {
                strncpy(m->csv_name, csv_name, sizeof(m->csv_name) - 1);
                m->csv_name[sizeof(m->csv_name) - 1] = '\0';
            }
        },
        true);
}
