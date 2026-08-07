#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure math for the swept-RSSI panadapter (host-testable; no radio/UI deps).

#define SPECTRUM_RSSI_FLOOR (-97.0f) // dBm mapped to bar height 0
#define SPECTRUM_RSSI_CEIL  (-60.0f) // dBm mapped to full bar height

// Shared with subghz_view_spectrum.h (NBINS / SPEC_H must match its values).
#define SPECTRUM_NBINS 128 // bins per sweep
#define SPECTRUM_BAR_H 26 // spectrum bar pixel height

// Center frequency (Hz) of bin i in a uniform sweep.
uint32_t spectrum_bin_freq(uint32_t band_start, uint32_t bin_step, size_t i);

// Quantise an RSSI dBm reading to a bar height in [0, height].
uint8_t spectrum_rssi_q(float dbm, uint8_t height);
