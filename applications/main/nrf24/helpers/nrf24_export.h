#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

int nrf24_export_format_csv(const uint8_t* levels, uint8_t n, char* out, size_t cap);
bool nrf24_settings_validate(uint32_t mode, uint32_t dwell, uint8_t* mode_out, uint16_t* dwell_out);

#ifndef NRF24_EXPORT_HOST_TEST
bool nrf24_export_scan(const uint8_t* levels, uint8_t n);
bool nrf24_settings_load(uint8_t* jam_mode, uint16_t* dwell_us);
bool nrf24_settings_save(uint8_t jam_mode, uint16_t dwell_us);
#endif
