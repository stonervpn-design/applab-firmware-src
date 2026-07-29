#include "wlan_station_map.h"

void wlan_station_map_reset(WlanStationMap* map) {
    map->ap_count = 0;
    map->tick_counter = 0;
    memset(map->aps, 0, sizeof(map->aps));
}

void wlan_station_map_tick(WlanStationMap* map) {
    map->tick_counter++;
}

void wlan_station_map_add(
    WlanStationMap* map,
    const uint8_t bssid[6],
    const uint8_t client_mac[6],
    int8_t rssi) {
    /* Find or create the AP. */
    int ap_idx = -1;
    for(uint8_t i = 0; i < map->ap_count; i++) {
        if(memcmp(map->aps[i].bssid, bssid, 6) == 0) {
            ap_idx = i;
            break;
        }
    }
    if(ap_idx < 0) {
        if(map->ap_count >= WLAN_SM_MAX_APS) return; /* AP table full */
        ap_idx = map->ap_count;
        memcpy(map->aps[ap_idx].bssid, bssid, 6);
        map->aps[ap_idx].client_count = 0;
        map->aps[ap_idx].first_seen_tick = map->tick_counter;
        map->ap_count++;
    }

    WlanStationAp* ap = &map->aps[ap_idx];

    /* Find or create the client under this AP. */
    int cli_idx = -1;
    for(uint8_t i = 0; i < ap->client_count; i++) {
        if(memcmp(ap->clients[i].mac, client_mac, 6) == 0) {
            cli_idx = i;
            break;
        }
    }
    if(cli_idx < 0) {
        if(ap->client_count >= WLAN_SM_MAX_CLIENTS) return; /* client table full */
        cli_idx = ap->client_count;
        memcpy(ap->clients[cli_idx].mac, client_mac, 6);
        ap->client_count++;
    }

    ap->clients[cli_idx].rssi = rssi;
    ap->clients[cli_idx].last_seen_tick = map->tick_counter;
}

uint8_t wlan_station_map_ap_count(const WlanStationMap* map) {
    return map->ap_count;
}

const WlanStationAp* wlan_station_map_ap_at(const WlanStationMap* map, uint8_t idx) {
    if(idx >= map->ap_count) return NULL;
    return &map->aps[idx];
}
