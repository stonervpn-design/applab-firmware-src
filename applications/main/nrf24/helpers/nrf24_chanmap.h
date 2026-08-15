#pragma once
#include <stdint.h>

#define NRF24_FUSION_CH 126

uint16_t nrf24_ch_to_mhz(uint8_t ch);
int      nrf24_wifi_ch_to_nrf(uint8_t wifi_ch);
void     nrf24_wifi_span(uint8_t wifi_ch, uint8_t* lo, uint8_t* hi);
int      nrf24_ble_adv_to_nrf(uint8_t adv_ch);
uint8_t  nrf24_dbm_to_pct(int8_t dbm);
uint16_t nrf24_rgb565_swap(uint16_t c);
