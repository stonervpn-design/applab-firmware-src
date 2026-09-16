#pragma once

#include <stdbool.h>
#include <stdint.h>
/* NimBLE migration: the public structs below still expose the Bluedroid *value*
 * types (esp_bd_addr_t, esp_ble_addr_type_t, esp_bt_uuid_t) and the
 * ESP_UUID_LEN_* macros so that every consumer (scenes/views, race detector,
 * auto-walk log) keeps compiling unchanged. Those types/macros live in
 * esp_bt_defs.h, which is pure typedefs/macros and does NOT pull in the
 * Bluedroid GAP/GATTC *host* API (esp_gap_ble_api.h / esp_gattc_api.h) that we
 * no longer use. The .c now drives the NimBLE host and converts NimBLE
 * ble_addr_t / ble_uuid_any_t into these types at the boundary. */
#include "ble_compat_defs.h"

#define BLE_WALK_MAX_DEVICES    32
#define BLE_WALK_MAX_SERVICES   16
#define BLE_WALK_MAX_CHARS      32
#define BLE_WALK_MAX_VALUE_LEN  128

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    char name[32];
    uint8_t adv_data[31];
    uint8_t adv_data_len;
    uint8_t scan_rsp_data[31];
    uint8_t scan_rsp_len;
} BleWalkDevice;

typedef struct {
    esp_bt_uuid_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} BleWalkService;

typedef struct {
    esp_bt_uuid_t uuid;
    uint16_t handle;
    uint8_t properties;
} BleWalkChar;

// Lifecycle
bool ble_walk_hal_start(void);
void ble_walk_hal_stop(void);

// Scanning
bool ble_walk_hal_start_scan(void);
void ble_walk_hal_stop_scan(void);
bool ble_walk_hal_is_scanning(void);
void ble_walk_hal_clear_devices(void); // reset the shared device table (call on detector entry)
BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count);

// GATT Client
// `abort_flag` is optional; pass a pointer to a volatile bool that the caller
// flips to abort the connect early. NULL ok.
bool ble_walk_hal_connect(BleWalkDevice* device, volatile bool* abort_flag);
void ble_walk_hal_disconnect(void);
bool ble_walk_hal_is_connected(void);

bool ble_walk_hal_discover_services(void);
bool ble_walk_hal_services_ready(void);
BleWalkService* ble_walk_hal_get_services(uint16_t* count);

bool ble_walk_hal_discover_chars(BleWalkService* service);
bool ble_walk_hal_chars_ready(void);
BleWalkChar* ble_walk_hal_get_chars(uint16_t* count);

bool ble_walk_hal_read_char(uint16_t handle);
bool ble_walk_hal_read_ready(void);
uint8_t* ble_walk_hal_get_read_value(uint16_t* len);
uint8_t ble_walk_hal_get_read_status(void);

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len);

#ifndef PUBLIC_BUILD
// ---------------------------------------------------------------------------
// GATT fuzz + connection-DoS primitives (private-only: new attack surface).
// All operate against the currently-connected/locked target only (the flood
// start is the sole exception — it takes its own target and drives its own
// connect/disconnect churn on an internal worker thread).
// ---------------------------------------------------------------------------

// Fire-and-forget write (Write Without Response). No completion callback, no
// flow control — fastest way to hammer a characteristic with fuzz payloads.
bool ble_walk_hal_write_char_norsp(uint16_t handle, const uint8_t* data, uint16_t len);

// Write Long Characteristic Values (queued/prepared-write procedure). Use for
// payloads longer than the negotiated ATT_MTU - 3, or to specifically exercise
// a peer's prepare-write/execute-write queue handling during fuzzing.
bool ble_walk_hal_write_char_long(uint16_t handle, uint16_t offset, const uint8_t* data, uint16_t len);

// MTU fuzzing: sets our preferred MTU then re-triggers the ATT MTU exchange
// on the current connection. Negotiated result is captured by the GAP event
// callback and readable via ble_walk_hal_get_negotiated_mtu().
bool ble_walk_hal_fuzz_mtu(uint16_t preferred_mtu);
uint16_t ble_walk_hal_get_negotiated_mtu(void);

// Connection-flood DoS: repeatedly connects to `target`'s address, holds the
// link open briefly (hold_count-scaled dwell, 0 == disconnect immediately for
// maximum churn rate), then disconnects and reconnects — driven by an
// internal worker thread + stop flag until ble_walk_hal_dos_flood_stop() is
// called. ble_walk_hal_dos_counter() reports completed connect cycles so far.
// Requires the HAL to already be started (ble_walk_hal_start()); operates
// only on target->addr / target->addr_type, independent of any other
// in-progress connection this HAL instance holds.
bool ble_walk_hal_dos_connect_flood_start(const BleWalkDevice* target, uint16_t hold_count);
void ble_walk_hal_dos_flood_stop(void);
uint32_t ble_walk_hal_dos_counter(void);
#endif // PUBLIC_BUILD
