#include "wlan_spectrum.h"

static uint8_t spec_level(uint16_t count, int8_t peak) {
    if(count == 0) return 0;
    uint16_t c = count > 20 ? 20 : count;
    int level = 1 + (int)(c / 2);       // 1..11
    if(peak > -60) level += 4;
    else if(peak > -80) level += 2;
    if(level > 15) level = 15;
    return (uint8_t)level;
}

void wlan_spectrum_reset(WlanSpectrum* s) {
    memset(s, 0, sizeof(*s));
    for(int i = 0; i < SPEC_CHANNELS; i++) s->cur_peak[i] = -128;
}

void wlan_spectrum_set_dwell(WlanSpectrum* s, uint8_t channel,
                             uint16_t frame_count, int8_t peak_rssi) {
    if(channel < 1 || channel > SPEC_CHANNELS) return;
    s->cur_count[channel - 1] = frame_count;
    s->cur_peak[channel - 1] = peak_rssi;
}

void wlan_spectrum_commit_sweep(WlanSpectrum* s) {
    for(int i = 0; i < SPEC_CHANNELS; i++) {
        s->rows[s->head][i] = spec_level(s->cur_count[i], s->cur_peak[i]);
    }
    s->head = (uint8_t)((s->head + 1) % SPEC_ROWS);
    if(s->filled < SPEC_ROWS) s->filled++;
    for(int i = 0; i < SPEC_CHANNELS; i++) { s->cur_count[i] = 0; s->cur_peak[i] = -128; }
}

uint8_t wlan_spectrum_live_level(const WlanSpectrum* s, uint8_t channel) {
    if(channel < 1 || channel > SPEC_CHANNELS) return 0;
    return spec_level(s->cur_count[channel - 1], s->cur_peak[channel - 1]);
}

uint8_t wlan_spectrum_cell(const WlanSpectrum* s, uint8_t row_from_top, uint8_t channel) {
    if(channel < 1 || channel > SPEC_CHANNELS) return 0;
    if(row_from_top >= s->filled) return 0;
    // newest row = head-1; walk backwards
    int idx = (int)s->head - 1 - (int)row_from_top;
    idx %= SPEC_ROWS;
    if(idx < 0) idx += SPEC_ROWS;
    return s->rows[idx][channel - 1];
}
