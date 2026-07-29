#include "wlan_spectrum_view.h"
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <furi.h>
#include <input/input.h>
#include <stdio.h>

static void draw_dither_cell(Canvas* canvas, int x, int y, int w, int h, uint8_t level, bool dim) {
    // level 0..15 -> 0..3 density buckets; dim halves it.
    uint8_t b = level / 4; // 0..3 (15/4=3)
    if(dim && b) b--;
    if(b == 0) return;
    for(int yy = 0; yy < h; yy++) {
        for(int xx = 0; xx < w; xx++) {
            // ordered-ish dither: denser bucket -> more set pixels
            int r = (xx + yy) & 3;
            if(r < b) canvas_draw_dot(canvas, x + xx, y + yy);
        }
    }
}

static bool ch_matches(const WlanSpectrumViewModel* m, uint8_t ch) {
    if(m->filter == SpecFilterVuln) return (m->vuln_mask >> (ch - 1)) & 1;
    if(m->filter == SpecFilterHidden) return (m->hidden_mask >> (ch - 1)) & 1;
    return true;
}

static void spectrum_draw(Canvas* canvas, void* model) {
    WlanSpectrumViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // Header line
    char hdr[32];
    const char* fname = m->filter == SpecFilterVuln    ? "VULN" :
                        m->filter == SpecFilterHidden  ? "HIDDEN" :
                                                         "ALL";
    snprintf(
        hdr, sizeof(hdr), "%s CH%u %s%s", m->locked ? "LOCK" : "HOP",
        (unsigned)m->cur_channel, fname, m->paused ? " ||" : "");
    canvas_draw_str(canvas, 2, 8, hdr);

    // Column geometry: 13 channels across 128 px -> ~9 px each starting x=4.
    const int x0 = 4, cw = 9;

    // Top strip live bars (y 10..14, 5 px tall).
    if(m->spec) {
        for(uint8_t ch = 1; ch <= SPEC_CHANNELS; ch++) {
            uint8_t lvl = wlan_spectrum_live_level(m->spec, ch);
            int bh = (lvl * 5) / 15;
            int x = x0 + (ch - 1) * cw;
            bool dim = !ch_matches(m, ch);
            if(!dim)
                canvas_draw_box(canvas, x, 14 - bh, cw - 1, bh);
            else if(bh)
                canvas_draw_frame(canvas, x, 14 - bh, cw - 1, bh);
            if(ch == m->cur_channel) canvas_draw_line(canvas, x, 15, x + cw - 2, 15);
        }
    }

    // Waterfall body y 17..63 -> up to 46 rows; we have 40; 1 px/row.
    if(m->spec) {
        int rows = SPEC_ROWS < 46 ? SPEC_ROWS : 46;
        for(int r = 0; r < rows; r++) {
            int y = 17 + r;
            for(uint8_t ch = 1; ch <= SPEC_CHANNELS; ch++) {
                uint8_t lvl = wlan_spectrum_cell(m->spec, (uint8_t)r, ch);
                int x = x0 + (ch - 1) * cw;
                draw_dither_cell(canvas, x, y, cw - 1, 1, lvl, !ch_matches(m, ch));
            }
        }
    }
}

static bool spectrum_input(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    switch(event->key) {
    case InputKeyLeft:
        view_dispatcher_send_custom_event(vd, SpectrumEvtLeft);
        return true;
    case InputKeyRight:
        view_dispatcher_send_custom_event(vd, SpectrumEvtRight);
        return true;
    case InputKeyOk:
        view_dispatcher_send_custom_event(vd, SpectrumEvtOk);
        return true;
    case InputKeyUp:
    case InputKeyDown:
        view_dispatcher_send_custom_event(vd, SpectrumEvtFilter);
        return true;
    default:
        return false; // Back falls through to the scene-manager back handler
    }
}

View* wlan_spectrum_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanSpectrumViewModel));
    view_set_draw_callback(v, spectrum_draw);
    view_set_input_callback(v, spectrum_input);
    return v;
}

void wlan_spectrum_view_free(View* v) {
    view_free(v);
}
