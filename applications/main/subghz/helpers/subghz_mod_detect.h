#pragma once
#include "signal_fingerprint.h" // SignalMod
#include <stdint.h>
#include <stddef.h>

// On-device OOK/FSK modulation detection for the Sub-GHz frequency analyzer.
// General capture-quality feature (not car-specific) — ships in all variants.

typedef struct {
    SignalMod mod; // SIG_MOD_AM (OOK) | SIG_MOD_FM (FSK) | SIG_MOD_UNK
    uint8_t confidence; // 0..100
    int8_t floor_dbm; // low-percentile RSSI of the window
    int8_t peak_dbm; // high-percentile RSSI of the window
} SubGhzModDetect;

// PURE (subghz_mod_classify.c): classify a window of RSSI samples in dBm.
// No hardware, no globals — host-testable.
SubGhzModDetect subghz_mod_classify(const int8_t* rssi_dbm, size_t count);

// HW (subghz_mod_detect.c): tune to freq, sample RSSI over the detection window,
// then call subghz_mod_classify(). Caller (analyzer worker) owns the radio.
SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz);
