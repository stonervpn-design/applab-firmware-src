#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * BLE spam detector: alerts on Continuity/FastPair/SwiftPair advertisement floods
 * and random-MAC flooding.
 *
 * Pure eval function (no firmware dependencies).
 */

#define BLE_SPAM_ALERT        25u   // spam-payload adverts per window
#define BLE_MAC_FLOOD_ALERT   40u   // distinct locally-administered MACs per window
#define BLE_SPAM_WINDOW_MS    1000u

#define BLE_CID_APPLE     0x004Cu    // Apple Continuity / Nearby
#define BLE_CID_MICROSOFT 0x0006u    // Microsoft SwiftPair
#define BLE_CID_SAMSUNG   0x0075u    // Samsung SmartTag
#define BLE_UUID_FASTPAIR 0xFE2Cu    // Google Fast Pair

/**
 * BLE advertisement record for spam detection.
 *
 * Fields:
 *   addr[6] - BLE MAC address
 *   is_random_mac - true if locally-administered bit set (addr[0] & 0x02)
 *   company_id - manufacturer-data company ID (0 if none)
 *   service_uuid - GATT service UUID (0 if none)
 *   ts_ms - timestamp in milliseconds (monotonic)
 */
typedef struct {
    uint8_t  addr[6];
    bool     is_random_mac;
    uint16_t company_id;
    uint16_t service_uuid;
    uint32_t ts_ms;
} DefenderBleSpamRec;

/**
 * Spam evaluation result.
 *
 * Fields:
 *   alert - true if spam_total >= spam_alert OR distinct_random_macs >= mac_flood_alert
 *   continuity - count of Apple Continuity adverts (company_id == 0x004C) in window
 *   fastpair - count of Google Fast Pair adverts (service_uuid == 0xFE2C) in window
 *   swiftpair - count of Microsoft SwiftPair adverts (company_id == 0x0006) in window
 *   distinct_random_macs - count of distinct random MACs (is_random_mac==true) in window
 */
typedef struct {
    bool     alert;
    uint16_t continuity;
    uint16_t fastpair;
    uint16_t swiftpair;
    uint16_t distinct_random_macs;
} DefenderBleSpamResult;

/**
 * Evaluate BLE spam in a sliding window.
 *
 * Pure function: no side effects except writing to *out.
 *
 * Algorithm:
 *   1. Filter records to in-window: (rec.ts_ms > now_ms - window_ms) && (rec.ts_ms <= now_ms)
 *   2. Count spam types: continuity, fastpair, swiftpair
 *   3. Count distinct random MACs (up to 64 seen)
 *   4. Set alert = (continuity+fastpair+swiftpair >= spam_alert) ||
 *                   (distinct_random_macs >= mac_flood_alert)
 *
 * Parameters:
 *   recs - array of BLE advertisement records
 *   n - number of records
 *   now_ms - current time in milliseconds
 *   window_ms - window duration in milliseconds
 *   spam_alert - threshold for spam-payload rate (default 25)
 *   mac_flood_alert - threshold for random-MAC rate (default 40)
 *   out - result structure (must not be NULL)
 */
void defender_blespam_eval(
    const DefenderBleSpamRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    uint32_t spam_alert,
    uint32_t mac_flood_alert,
    DefenderBleSpamResult* out);
