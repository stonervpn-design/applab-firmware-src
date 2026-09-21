#include "nrf24_chanmap.h"

uint16_t nrf24_ch_to_mhz(uint8_t ch) {
    return (uint16_t)(2400 + ch);
}

int nrf24_wifi_ch_to_nrf(uint8_t wifi_ch) {
    if(wifi_ch >= 1 && wifi_ch <= 13) return 12 + (wifi_ch - 1) * 5;
    if(wifi_ch == 14) return 84; /* 2484 MHz, non-linear */
    return -1;
}

void nrf24_wifi_span(uint8_t wifi_ch, uint8_t* lo, uint8_t* hi) {
    int c = nrf24_wifi_ch_to_nrf(wifi_ch);
    if(c < 0) { *lo = 1; *hi = 0; return; } /* empty: lo>hi */
    int l = c - 11, h = c + 11;
    if(l < 0) l = 0;
    if(h > NRF24_FUSION_CH - 1) h = NRF24_FUSION_CH - 1;
    *lo = (uint8_t)l; *hi = (uint8_t)h;
}

int nrf24_ble_adv_to_nrf(uint8_t adv_ch) {
    switch(adv_ch) {
    case 37: return 2;
    case 38: return 26;
    case 39: return 80;
    default: return -1;
    }
}

uint8_t nrf24_dbm_to_pct(int8_t dbm) {
    int d = dbm;
    if(d <= -95) return 0;
    if(d >= -35) return 100;
    return (uint8_t)(((d + 95) * 100) / 60);
}

uint16_t nrf24_rgb565_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}
