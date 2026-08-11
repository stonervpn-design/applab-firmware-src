#pragma once
#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"

// Swept-RSSI panadapter view (PRIVATE builds). Renders a per-bin RSSI bar
// spectrum + a scrolling 1-bit waterfall + a peak label on the 128x64 display.
// A sweep worker feeds completed rows via subghz_view_spectrum_push_row.

#define SUBGHZ_SPECTRUM_NBINS   128 // one column per bin
#define SUBGHZ_SPECTRUM_WF_ROWS 24 // waterfall history rows
#define SUBGHZ_SPECTRUM_SPEC_H  26 // spectrum bar pixel range (0..SPEC_H)

typedef struct SubGhzViewSpectrum SubGhzViewSpectrum;

typedef void (*SubGhzViewSpectrumCallback)(SubGhzCustomEvent event, void* context);

SubGhzViewSpectrum* subghz_view_spectrum_alloc(void);
void subghz_view_spectrum_free(SubGhzViewSpectrum* instance);
View* subghz_view_spectrum_get_view(SubGhzViewSpectrum* instance);
void subghz_view_spectrum_set_callback(
    SubGhzViewSpectrum* instance,
    SubGhzViewSpectrumCallback callback,
    void* context);

// Push one completed sweep row. `bins` holds NBINS quantised heights (0..SPEC_H).
void subghz_view_spectrum_push_row(
    SubGhzViewSpectrum* instance,
    uint32_t band_start,
    uint32_t bin_step,
    const uint8_t* bins,
    size_t nbins,
    uint32_t peak_freq,
    float peak_rssi);

// Current peak frequency (Hz) for tune-to-peak; 0 if no data yet.
uint32_t subghz_view_spectrum_get_peak_freq(SubGhzViewSpectrum* instance);

// True only when the latest sweep's peak stands clearly above the noise floor
// (peak bar rises off the absolute RSSI floor AND clears the sweep's mean floor
// by a margin). Used to gate tune-to-peak so a noise-only sweep never retunes.
bool subghz_view_spectrum_peak_is_signal(SubGhzViewSpectrum* instance);
