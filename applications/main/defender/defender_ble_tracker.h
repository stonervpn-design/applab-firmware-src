#pragma once

#include <stdint.h>
#include "../ble_spam/ble_tracker_hal.h"

/**
 * Poll tracker devices for unwanted-tracker alerts.
 *
 * Iterates through devices, computes follow state via tracker_follow_state(),
 * decides via should_alert(), and returns the index of the strongest fresh
 * offender (highest RSSI among alerting devices), or -1 if none.
 *
 * @param devs       Array of TrackerDevice
 * @param count      Number of devices
 * @param now_ms     Current time in milliseconds (typically furi_get_tick())
 * @return           Index of strongest offender, or -1
 */
int defender_ble_tracker_poll(const TrackerDevice* devs, uint16_t count, uint32_t now_ms);
