#include "subghz_view_spectrum.h"
#include <gui/elements.h>
#include <input/input.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

// Swept-RSSI panadapter view. Layout (128x64, 1-bit):
//   header  y0..7   : "Spectrum" + peak MHz/dBm
//   spectrum y11..37: per-bin RSSI bars (baseline 37)
//   waterfall y41.. : thresholded scrolling rows (drawn in a later task)

#define SP_WF_BYTES  ((SUBGHZ_SPECTRUM_NBINS + 7) / 8) // 16
#define SP_SPEC_BASE 37 // spectrum baseline y
#define SP_HOT       (SUBGHZ_SPECTRUM_SPEC_H / 3) // waterfall "carrier present" threshold

struct SubGhzViewSpectrum {
    View* view;
    SubGhzViewSpectrumCallback callback;
    void* context;
};

typedef struct {
    uint8_t spec[SUBGHZ_SPECTRUM_NBINS]; // bar heights 0..SPEC_H
    uint8_t wf[SUBGHZ_SPECTRUM_WF_ROWS][SP_WF_BYTES]; // 1-bit waterfall rows
    uint8_t wf_head; // next row to write (ring)
    uint32_t band_start;
    uint32_t bin_step;
    uint32_t peak_freq;
    float peak_rssi;
    uint16_t peak_bin;
    bool have_data;
} SubGhzViewSpectrumModel;

void subghz_view_spectrum_set_callback(
    SubGhzViewSpectrum* instance,
    SubGhzViewSpectrumCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

static void subghz_view_spectrum_fire(SubGhzViewSpectrum* instance, SubGhzCustomEvent event) {
    if(instance->callback) instance->callback(event, instance->context);
}

void subghz_view_spectrum_push_row(
    SubGhzViewSpectrum* instance,
    uint32_t band_start,
    uint32_t bin_step,
    const uint8_t* bins,
    size_t nbins,
    uint32_t peak_freq,
    float peak_rssi) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        SubGhzViewSpectrumModel * model,
        {
            size_t n = nbins < SUBGHZ_SPECTRUM_NBINS ? nbins : SUBGHZ_SPECTRUM_NBINS;
            memcpy(model->spec, bins, n);
            model->band_start = band_start;
            model->bin_step = bin_step;
            model->peak_freq = peak_freq;
            model->peak_rssi = peak_rssi;

            uint16_t pb = 0;
            uint8_t pv = 0;
            for(size_t i = 0; i < n; i++) {
                if(model->spec[i] > pv) {
                    pv = model->spec[i];
                    pb = (uint16_t)i;
                }
            }
            model->peak_bin = pb;

            // Append one thresholded waterfall row at the ring head.
            uint8_t* row = model->wf[model->wf_head];
            memset(row, 0, SP_WF_BYTES);
            for(size_t i = 0; i < n; i++) {
                if(model->spec[i] > SP_HOT) row[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
            model->wf_head = (uint8_t)((model->wf_head + 1) % SUBGHZ_SPECTRUM_WF_ROWS);
            model->have_data = true;
        },
        true);
}

uint32_t subghz_view_spectrum_get_peak_freq(SubGhzViewSpectrum* instance) {
    furi_assert(instance);
    uint32_t f = 0;
    with_view_model(
        instance->view, SubGhzViewSpectrumModel * model, { f = model->peak_freq; }, false);
    return f;
}

static void subghz_view_spectrum_draw(Canvas* canvas, SubGhzViewSpectrumModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 0, 7, "Spectrum");
    if(model->have_data) {
        char lbl[24];
        snprintf(
            lbl,
            sizeof(lbl),
            "%lu.%02lu %d",
            (unsigned long)(model->peak_freq / 1000000UL),
            (unsigned long)((model->peak_freq % 1000000UL) / 10000UL),
            (int)model->peak_rssi);
        canvas_draw_str_aligned(canvas, 127, 0, AlignRight, AlignTop, lbl);
    }
    canvas_draw_line(canvas, 0, 9, 127, 9);

    if(!model->have_data) {
        canvas_draw_str(canvas, 2, 30, "sweeping...");
        return;
    }

    for(size_t i = 0; i < SUBGHZ_SPECTRUM_NBINS; i++) {
        uint8_t h = model->spec[i];
        if(h > SUBGHZ_SPECTRUM_SPEC_H) h = SUBGHZ_SPECTRUM_SPEC_H;
        if(h) canvas_draw_line(canvas, (int)i, SP_SPEC_BASE, (int)i, SP_SPEC_BASE - h);
    }
    // Peak tick at the top of the spectrum zone.
    canvas_draw_line(canvas, (int)model->peak_bin, 11, (int)model->peak_bin, 13);

    canvas_draw_line(canvas, 0, 39, 127, 39); // spectrum / waterfall separator

    // Waterfall: newest sweep at top (y=41), scrolling down; 1-bit thresholded.
    for(size_t r = 0; r < SUBGHZ_SPECTRUM_WF_ROWS; r++) {
        int y = 41 + (int)r;
        if(y > 60) break;
        size_t src =
            (model->wf_head + SUBGHZ_SPECTRUM_WF_ROWS - 1 - r) % SUBGHZ_SPECTRUM_WF_ROWS;
        const uint8_t* row = model->wf[src];
        for(size_t i = 0; i < SUBGHZ_SPECTRUM_NBINS; i++) {
            if(row[i >> 3] & (uint8_t)(1u << (i & 7))) canvas_draw_dot(canvas, (int)i, y);
        }
    }
    // Persistent peak-column marker down the waterfall (dashed).
    for(int y = 41; y <= 60; y += 4) canvas_draw_dot(canvas, (int)model->peak_bin, y);

    canvas_draw_str(canvas, 0, 63, "OK:Tune  L/R:Band");
}

static bool subghz_view_spectrum_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewSpectrum* instance = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;
    switch(event->key) {
    case InputKeyLeft:
        subghz_view_spectrum_fire(instance, SubGhzCustomEventViewSpectrumBandDown);
        return true;
    case InputKeyRight:
        subghz_view_spectrum_fire(instance, SubGhzCustomEventViewSpectrumBandUp);
        return true;
    case InputKeyOk:
        subghz_view_spectrum_fire(instance, SubGhzCustomEventViewSpectrumTune);
        return true;
    default:
        return false; // Back -> scene-manager navigation exits the scene
    }
}

SubGhzViewSpectrum* subghz_view_spectrum_alloc(void) {
    SubGhzViewSpectrum* instance = malloc(sizeof(SubGhzViewSpectrum));
    instance->callback = NULL;
    instance->context = NULL;
    instance->view = view_alloc();
    view_allocate_model(
        instance->view, ViewModelTypeLocking, sizeof(SubGhzViewSpectrumModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, (ViewDrawCallback)subghz_view_spectrum_draw);
    view_set_input_callback(instance->view, subghz_view_spectrum_input);

    with_view_model(
        instance->view,
        SubGhzViewSpectrumModel * model,
        {
            memset(model, 0, sizeof(SubGhzViewSpectrumModel));
            model->have_data = false;
        },
        true);

    return instance;
}

void subghz_view_spectrum_free(SubGhzViewSpectrum* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* subghz_view_spectrum_get_view(SubGhzViewSpectrum* instance) {
    furi_assert(instance);
    return instance->view;
}
