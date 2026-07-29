#include "../applications/main/wlan_app/wlan_station_map.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void mkmac(uint8_t m[6], uint8_t last) {
    m[0] = 0x02; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0; m[5] = last;
}

int main(void) {
    WlanStationMap map;
    wlan_station_map_reset(&map);
    assert(wlan_station_map_ap_count(&map) == 0);

    /* Add first AP with one client. */
    uint8_t ap1[6], cli1[6], cli2[6];
    mkmac(ap1, 0x01);
    mkmac(cli1, 0x11);
    mkmac(cli2, 0x12);

    wlan_station_map_add(&map, ap1, cli1, -50);
    assert(wlan_station_map_ap_count(&map) == 1);
    assert(map.aps[0].client_count == 1);
    assert(memcmp(map.aps[0].bssid, ap1, 6) == 0);
    assert(memcmp(map.aps[0].clients[0].mac, cli1, 6) == 0);
    assert(map.aps[0].clients[0].rssi == -50);

    /* Add second client to same AP. */
    wlan_station_map_add(&map, ap1, cli2, -60);
    assert(wlan_station_map_ap_count(&map) == 1);
    assert(map.aps[0].client_count == 2);

    /* Update first client RSSI (no dup). */
    wlan_station_map_add(&map, ap1, cli1, -55);
    assert(map.aps[0].client_count == 2);
    assert(map.aps[0].clients[0].rssi == -55);

    /* Add second AP. */
    uint8_t ap2[6];
    mkmac(ap2, 0x02);
    wlan_station_map_add(&map, ap2, cli1, -70);
    assert(wlan_station_map_ap_count(&map) == 2);
    assert(map.aps[1].client_count == 1);

    /* Tick counter increments. */
    assert(map.tick_counter == 0);
    wlan_station_map_tick(&map);
    assert(map.tick_counter == 1);

    /* first_seen_tick recorded at add time (ap2 added at tick 0). */
    assert(map.aps[1].first_seen_tick == 0);

    /* Fill past AP capacity: should cap at WLAN_SM_MAX_APS. */
    for(int i = 2; i < WLAN_SM_MAX_APS + 5; i++) {
        uint8_t api[6];
        mkmac(api, (uint8_t)i);
        uint8_t cli[6];
        mkmac(cli, (uint8_t)(0x80 + i));
        wlan_station_map_add(&map, api, cli, -40);
    }
    assert(wlan_station_map_ap_count(&map) == WLAN_SM_MAX_APS);

    /* Fill clients past capacity for one AP from a fresh map: should cap. */
    uint8_t ap_test[6];
    mkmac(ap_test, 0x99);
    WlanStationMap fresh;
    wlan_station_map_reset(&fresh);
    wlan_station_map_add(&fresh, ap_test, cli1, -45);
    for(int i = 1; i < WLAN_SM_MAX_CLIENTS + 5; i++) {
        uint8_t cli[6];
        mkmac(cli, (uint8_t)(0xA0 + i));
        wlan_station_map_add(&fresh, ap_test, cli, -45);
    }
    assert(fresh.aps[0].client_count == WLAN_SM_MAX_CLIENTS);

    /* ap_at bounds: valid returns non-NULL, OOB returns NULL. */
    assert(wlan_station_map_ap_at(&map, 0) != NULL);
    assert(wlan_station_map_ap_at(&map, WLAN_SM_MAX_APS) == NULL);

    printf("test_wlan_station_map OK\n");
    return 0;
}
