#pragma once
/**
 * @file wlan_station_map.h
 * Bounded, dependency-free BSSID -> {client MAC} association map.
 *
 * Fed by a passive 802.11 promiscuous sniffer (Station Scanner scene): each
 * observed data frame yields a (bssid, client_mac, rssi) tuple derived from the
 * to_ds/from_ds addressing bits. Static storage only (no heap, no PSRAM).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define WLAN_SM_MAX_APS 16
#define WLAN_SM_MAX_CLIENTS 8 /* per AP */

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint16_t last_seen_tick;
} WlanStationClient;

typedef struct {
    uint8_t bssid[6];
    WlanStationClient clients[WLAN_SM_MAX_CLIENTS];
    uint8_t client_count;
    uint16_t first_seen_tick;
} WlanStationAp;

typedef struct {
    WlanStationAp aps[WLAN_SM_MAX_APS];
    uint8_t ap_count;
    uint16_t tick_counter;
} WlanStationMap;

/* Lock-free SPSC ring entry: RX callback (producer) -> tick consumer. */
typedef struct {
    uint8_t bssid[6];
    uint8_t client_mac[6];
    int8_t rssi;
} WlanStationRxEvt;

/** Reset map to empty state. */
void wlan_station_map_reset(WlanStationMap* map);

/** Increment the tick counter (call once per second of scanning). */
void wlan_station_map_tick(WlanStationMap* map);

/**
 * Insert or update (bssid, client_mac, rssi).
 * Adds the AP if unseen and there is room; adds the client under that AP if
 * unseen and there is room. Always refreshes rssi + last_seen_tick when the
 * (AP, client) pair is present. Silently drops when either table is full.
 */
void wlan_station_map_add(
    WlanStationMap* map,
    const uint8_t bssid[6],
    const uint8_t client_mac[6],
    int8_t rssi);

/** Number of APs currently tracked. */
uint8_t wlan_station_map_ap_count(const WlanStationMap* map);

/** AP at index, or NULL if idx is out of range. */
const WlanStationAp* wlan_station_map_ap_at(const WlanStationMap* map, uint8_t idx);
