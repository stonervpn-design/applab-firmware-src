#include "defender_waterfall_view.h"
#include <gui/canvas.h>
#include <gui/view.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

/**
 * Draw callback: render scrolling waterfall + label + peak marker.
 * Uses a locking view model (DefenderWaterfallModel), so the callback receives
 * the model pointer directly.
 */
static void defender_waterfall_draw_callback(Canvas* canvas, void* _model) {
    DefenderWaterfallModel* m = (DefenderWaterfallModel*)_model;

    canvas_draw_frame(canvas, 0, 0, 128, 64);

    // Draw label at top
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, m->label);

    // Draw waterfall: WATERFALL_HISTW columns × up to WATERFALL_MAXBINS rows.
    // Each column is 2 pixels wide; newest on the right (scrolls left).
    for(int col = 0; col < WATERFALL_HISTW; col++) {
        int hist_idx = (m->hist_head + col) % WATERFALL_HISTW;
        int x_start = col * 2;

        for(uint8_t bin = 0; bin < m->nbins && bin < WATERFALL_MAXBINS; bin++) {
            uint8_t intensity = m->hist[hist_idx][bin];
            if(intensity >= 128) {
                int y = 10 + bin; // start below label
                if(y < 64) {
                    canvas_draw_dot(canvas, x_start, y);
                    if(x_start + 1 < 128) {
                        canvas_draw_dot(canvas, x_start + 1, y);
                    }
                }
            }
        }
    }

    // Draw peak marker.
    if(m->peak_bin >= 0 && m->peak_bin < m->nbins) {
        int y_peak = 10 + m->peak_bin;
        if(y_peak < 64) {
            canvas_draw_str_aligned(canvas, 120, y_peak, AlignRight, AlignTop, "*");
        }
    }
}

static bool defender_waterfall_input_callback(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false; // display-only; input handled by the scene via its override
}

View* defender_waterfall_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(DefenderWaterfallModel));

    DefenderWaterfallModel* m = view_get_model(view);
    memset(m, 0, sizeof(DefenderWaterfallModel));
    m->nbins = WATERFALL_MAXBINS;
    snprintf(m->label, sizeof(m->label), "Waterfall");
    view_commit_model(view, false);

    view_set_draw_callback(view, defender_waterfall_draw_callback);
    view_set_input_callback(view, defender_waterfall_input_callback);
    return view;
}

void defender_waterfall_view_free(View* view) {
    furi_check(view);
    view_free(view);
}

void defender_waterfall_view_push(
    View* view,
    const uint8_t* col,
    uint8_t nbins,
    int8_t peak_bin) {
    furi_check(view);

    DefenderWaterfallModel* m = view_get_model(view);

    // Advance the ring head, then write the new column at the head.
    m->hist_head = (m->hist_head + 1) % WATERFALL_HISTW;

    if(col) {
        uint8_t copy_bins = (nbins < WATERFALL_MAXBINS) ? nbins : WATERFALL_MAXBINS;
        memcpy(m->hist[m->hist_head], col, copy_bins);
        if(copy_bins < WATERFALL_MAXBINS) {
            memset(&m->hist[m->hist_head][copy_bins], 0, WATERFALL_MAXBINS - copy_bins);
        }
    } else {
        memset(m->hist[m->hist_head], 0, WATERFALL_MAXBINS);
    }

    m->nbins = (nbins < WATERFALL_MAXBINS) ? nbins : WATERFALL_MAXBINS;
    m->peak_bin = peak_bin;
    view_commit_model(view, true);
}

void defender_waterfall_view_set_label(View* view, const char* label) {
    furi_check(view);
    DefenderWaterfallModel* m = view_get_model(view);
    snprintf(m->label, sizeof(m->label), "%s", label);
    view_commit_model(view, true);
}
