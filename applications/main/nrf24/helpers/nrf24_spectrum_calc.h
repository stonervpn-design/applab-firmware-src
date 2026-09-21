#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NRF24_DWELL_PRESET_COUNT 5
extern const uint16_t nrf24_dwell_presets[NRF24_DWELL_PRESET_COUNT];

uint8_t nrf24_maxhold_update(uint8_t peak, uint8_t level);
int nrf24_channel_to_x(uint8_t ch, int width, uint8_t channels);
bool nrf24_channel_is_wifi_marker(uint8_t ch);
bool nrf24_channel_is_ble_marker(uint8_t ch);
uint16_t nrf24_channel_to_mhz(uint8_t ch);
uint8_t nrf24_dwell_next(uint8_t idx);
