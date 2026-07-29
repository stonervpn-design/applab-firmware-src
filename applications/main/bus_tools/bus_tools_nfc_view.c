#include "bus_tools_nfc_view.h"
#include "furi_hal/st25r3916/st25r3916_nfca.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void bus_tools_nfc_view_draw(Canvas* canvas, void* model) {
    BusToolsNfcViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "ST25R NFC Read");
    canvas_set_font(canvas, FontSecondary);

    if(m->init_ok != (int)St25rNfcaOk) {
        canvas_draw_str(canvas, 2, 26, "init:");
        canvas_draw_str(canvas, 34, 26, st25r3916_nfca_status_str((St25rNfcaStatus)m->init_ok));
        canvas_draw_str(canvas, 2, 40, "check unit @0x50 / Grove");
        return;
    }

    canvas_draw_str(canvas, 2, 22, "field on - tap a card");
    canvas_draw_line(canvas, 0, 25, 127, 25);

    if(m->uid_len > 0) {
        char line[40];
        char* p = line;
        for(uint8_t i = 0; i < m->uid_len && (p - line) < (int)sizeof(line) - 3; i++) {
            p += snprintf(p, 4, "%02X", m->uid[i]);
        }
        canvas_draw_str(canvas, 2, 37, "UID:");
        canvas_draw_str(canvas, 24, 37, line);
        char meta[32];
        snprintf(meta, sizeof(meta), "ATQA %04X  SAK %02X", m->atqa, m->sak);
        canvas_draw_str(canvas, 2, 49, meta);
    } else {
        canvas_draw_str(canvas, 2, 40, st25r3916_nfca_status_str((St25rNfcaStatus)m->last_status));
    }

    char foot[24];
    snprintf(foot, sizeof(foot), "reads: %lu", (unsigned long)m->reads);
    canvas_draw_str(canvas, 2, 62, foot);
}

static bool bus_tools_nfc_view_input(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false;
}

View* bus_tools_nfc_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(BusToolsNfcViewModel));
    view_set_draw_callback(v, bus_tools_nfc_view_draw);
    view_set_input_callback(v, bus_tools_nfc_view_input);
    return v;
}

void bus_tools_nfc_view_free(View* v) {
    view_free(v);
}
