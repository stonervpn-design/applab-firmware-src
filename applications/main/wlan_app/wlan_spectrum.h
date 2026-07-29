#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SPEC_CHANNELS 13
#define SPEC_ROWS 40

typedef struct {
    uint16_t cur_count[SPEC_CHANNELS];
    int8_t   cur_peak[SPEC_CHANNELS];
    uint8_t  rows[SPEC_ROWS][SPEC_CHANNELS];
    uint8_t  head;
    uint8_t  filled;
} WlanSpectrum;

void wlan_spectrum_reset(WlanSpectrum* s);
void wlan_spectrum_set_dwell(WlanSpectrum* s, uint8_t channel,
                             uint16_t frame_count, int8_t peak_rssi);
void wlan_spectrum_commit_sweep(WlanSpectrum* s);
uint8_t wlan_spectrum_live_level(const WlanSpectrum* s, uint8_t channel);
uint8_t wlan_spectrum_cell(const WlanSpectrum* s, uint8_t row_from_top, uint8_t channel);
