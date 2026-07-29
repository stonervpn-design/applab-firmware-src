#pragma once

/* Minimal compat shim for the handful of Bluedroid value types still used in
 * this app's public structs (addresses, UUIDs). Under the NimBLE host config the
 * Bluedroid API headers (esp_gap_ble_api.h / esp_bt_defs.h) are not on the include
 * path, so we provide just the value types here. These are plain data types with
 * no host dependency, so they coexist fine with NimBLE. */

#include <stdint.h>

typedef uint8_t esp_bd_addr_t[6];

typedef enum {
    BLE_ADDR_TYPE_PUBLIC = 0x00,
    BLE_ADDR_TYPE_RANDOM = 0x01,
    BLE_ADDR_TYPE_RPA_PUBLIC = 0x02,
    BLE_ADDR_TYPE_RPA_RANDOM = 0x03,
} esp_ble_addr_type_t;

#define ESP_UUID_LEN_16  2
#define ESP_UUID_LEN_32  4
#define ESP_UUID_LEN_128 16

typedef struct {
    uint16_t len;
    union {
        uint16_t uuid16;
        uint32_t uuid32;
        uint8_t uuid128[ESP_UUID_LEN_128];
    } uuid;
} esp_bt_uuid_t;
