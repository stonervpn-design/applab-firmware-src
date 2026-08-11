#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BLE_ADV_MAX_U16 8
#define BLE_ADV_MAX_MFR 24

typedef struct {
    uint8_t  flags;
    bool     has_flags;
    uint16_t uuid16[BLE_ADV_MAX_U16];
    uint8_t  uuid16_count;
    uint8_t  uuid128[16];
    bool     has_uuid128;
    int8_t   tx_power;
    bool     has_tx_power;
    uint16_t mfr_id;
    bool     mfr_present;
    uint8_t  mfr_data[BLE_ADV_MAX_MFR];
    uint8_t  mfr_data_len;
} BleAdvInfo;

// Parses a raw BLE advertising-data buffer (attacker-controlled) into `out`.
// `out` is assumed pre-zeroed by the caller. Every read is bounded by `len`;
// a truncated/malformed AD structure terminates the walk without overreading.
void ble_adv_decode(const uint8_t* adv, uint8_t len, BleAdvInfo* out);
