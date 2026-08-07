#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Swept-RSSI panadapter worker (PRIVATE). Owns a thread that repeatedly steps a
// contiguous band of SPECTRUM_NBINS bins, reads RSSI per bin, and delivers each
// completed sweep to a row callback. Optimised vs the frequency analyzer:
// FS_AUTOCAL off + one manual calibration per band + a short settle.

typedef struct SubGhzSpectrumWorker SubGhzSpectrumWorker;

typedef void (*SubGhzSpectrumWorkerRowCallback)(
    void* ctx,
    uint32_t band_start,
    uint32_t bin_step,
    const uint8_t* bins, // SPECTRUM_NBINS quantised heights 0..SPECTRUM_BAR_H
    size_t nbins,
    uint32_t peak_freq,
    float peak_rssi);

SubGhzSpectrumWorker* subghz_spectrum_worker_alloc(void);
void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance);
void subghz_spectrum_worker_set_row_callback(
    SubGhzSpectrumWorker* instance,
    SubGhzSpectrumWorkerRowCallback cb,
    void* ctx);
void subghz_spectrum_worker_start(SubGhzSpectrumWorker* instance);
void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance);
bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance);

// Re-center the sweep (L/R band shift). Applied on the next sweep.
void subghz_spectrum_worker_set_band_start(SubGhzSpectrumWorker* instance, uint32_t band_start);
uint32_t subghz_spectrum_worker_get_band_start(SubGhzSpectrumWorker* instance);
uint32_t subghz_spectrum_worker_get_bin_step(SubGhzSpectrumWorker* instance);
