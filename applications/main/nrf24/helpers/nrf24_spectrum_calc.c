#include "nrf24_spectrum_calc.h"

const uint16_t nrf24_dwell_presets[NRF24_DWELL_PRESET_COUNT] = {130, 250, 500, 1000, 2000};

uint8_t nrf24_maxhold_update(uint8_t peak, uint8_t level) {
    return level > peak ? level : peak;
}
int nrf24_channel_to_x(uint8_t ch, int width, uint8_t channels) {
    if(channels == 0) return 0;
    return ((int)ch * width) / (int)channels;
}
bool nrf24_channel_is_wifi_marker(uint8_t ch) {
    return ch == 12 || ch == 37 || ch == 62; /* WiFi 1/6/11 */
}
bool nrf24_channel_is_ble_marker(uint8_t ch) {
    return ch == 2 || ch == 26 || ch == 80; /* BLE adv 37/38/39 */
}
uint16_t nrf24_channel_to_mhz(uint8_t ch) {
    return (uint16_t)(2400 + ch);
}
uint8_t nrf24_dwell_next(uint8_t idx) {
    return (uint8_t)((idx + 1) % NRF24_DWELL_PRESET_COUNT);
}
