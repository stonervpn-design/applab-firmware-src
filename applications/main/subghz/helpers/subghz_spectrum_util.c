#include "subghz_spectrum_util.h"

uint32_t spectrum_bin_freq(uint32_t band_start, uint32_t bin_step, size_t i) {
    return band_start + bin_step * (uint32_t)i;
}

uint8_t spectrum_rssi_q(float dbm, uint8_t height) {
    if(dbm <= SPECTRUM_RSSI_FLOOR) return 0;
    if(dbm >= SPECTRUM_RSSI_CEIL) return height;
    float f = (dbm - SPECTRUM_RSSI_FLOOR) / (SPECTRUM_RSSI_CEIL - SPECTRUM_RSSI_FLOOR);
    int h = (int)(f * (float)height + 0.5f);
    if(h < 0) h = 0;
    if(h > height) h = height;
    return (uint8_t)h;
}
