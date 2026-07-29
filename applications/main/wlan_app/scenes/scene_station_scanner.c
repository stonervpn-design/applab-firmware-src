#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include "../wlan_oui.h"
#include "../wlan_station_map.h"
#include <storage/storage.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define STATION_MIN_CH 1
#define STATION_MAX_CH 13
#define STATION_TICKS_PER_HOP 4 /* ~1s dwell per channel at the 250ms scene tick */

/* RX callback -> tick consumer, lock-free SPSC ring. */
#define STATION_RX_RING 32
// On-demand (were ~0.4 KB + ~1.4 KB always-resident BSS). s_ring is written by
// station_cb on the WiFi task; s_map is GUI-thread-only. Allocated on enter
// before the cb registers, freed on exit after it unregisters.
static WlanStationRxEvt* s_ring;
static volatile uint8_t s_head, s_tail;
static WlanStationMap* s_map;
static WlanOuiTable* s_oui;
static uint8_t s_cur_ch;
static uint8_t s_hop_tick;
static bool s_running;
/* Guards s_ring against a concurrent free() in on_exit while the WiFi RX task is
 * inside station_cb. Same pattern as scene_pmkid.c (p5-concurrency-hardening):
 * wlan_hal_set_promiscuous(false) syncs the HAL worker, NOT the WiFi RX task. */
static portMUX_TYPE s_station_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static bool station_mac_is_usable(const uint8_t* mac) {
    if(mac[0] & 0x01) return false; /* group bit: broadcast/multicast */
    return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0; /* not all-zero */
}

static void station_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_ring) return; /* buffer freed / not yet allocated */
    /* Only DATA frames carry a real AP<->client association. */
    if(type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    if(len < 24) return;

    const uint8_t* p = pkt->payload;
    uint16_t fc = (uint16_t)(p[0] | (p[1] << 8));
    uint8_t to_ds = (fc & 0x0100) ? 1 : 0;
    uint8_t from_ds = (fc & 0x0200) ? 1 : 0;

    const uint8_t* addr1 = &p[4];
    const uint8_t* addr2 = &p[10];

    const uint8_t* bssid;
    const uint8_t* client_mac;
    if(to_ds && !from_ds) {
        /* STA -> AP: addr1=BSSID, addr2=client */
        bssid = addr1;
        client_mac = addr2;
    } else if(!to_ds && from_ds) {
        /* AP -> STA: addr1=client, addr2=BSSID */
        bssid = addr2;
        client_mac = addr1;
    } else {
        /* IBSS (0/0) or WDS (1/1): no clean AP<->client pairing — skip. */
        return;
    }

    if(!station_mac_is_usable(bssid) || !station_mac_is_usable(client_mac)) return;

    /* Publish into the ring under the mux so on_exit can't free s_ring out from
     * under a callback that already passed the fast NULL check above. portEXIT
     * is the release barrier ordering payload stores before the head bump. */
    portENTER_CRITICAL(&s_station_ring_mux);
    if(s_ring) { /* authoritative re-check under the lock */
        uint8_t nh = (uint8_t)((s_head + 1) % STATION_RX_RING);
        if(nh != s_tail) { /* ring not full */
            memcpy(s_ring[s_head].bssid, bssid, 6);
            memcpy(s_ring[s_head].client_mac, client_mac, 6);
            s_ring[s_head].rssi = (int8_t)pkt->rx_ctrl.rssi;
            s_head = nh;
        }
    }
    portEXIT_CRITICAL(&s_station_ring_mux);
}

static uint16_t station_total_clients(void) {
    uint16_t total = 0;
    for(uint8_t i = 0; i < wlan_station_map_ap_count(s_map); i++) {
        total += wlan_station_map_ap_at(s_map, i)->client_count;
    }
    return total;
}

static void station_refresh_view(WlanApp* app) {
    char lines[STATIONS_VIEW_ROWS][32];
    uint8_t n = 0;
    uint8_t aps = wlan_station_map_ap_count(s_map);
    for(uint8_t i = 0; i < aps && n < STATIONS_VIEW_ROWS; i++) {
        const WlanStationAp* ap = wlan_station_map_ap_at(s_map, i);
        const char* vendor = wlan_oui_lookup(s_oui, ap->bssid);
        snprintf(
            lines[n], sizeof(lines[0]), "%02X%02X%02X(%u) %.10s",
            ap->bssid[3], ap->bssid[4], ap->bssid[5], ap->client_count,
            vendor ? vendor : "");
        n++;
    }
    wlan_stations_view_update(app->view_stations, lines, n, aps, station_total_clients());
}

static void station_write_csv(void) {
    uint8_t aps = wlan_station_map_ap_count(s_map);
    if(aps == 0) return;
    const char* dir = wlan_loot_session_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/stations.csv", dir);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "ap_bssid,client_mac,vendor,rssi,last_seen_s\n";
        storage_file_write(f, hdr, strlen(hdr));
        for(uint8_t i = 0; i < aps; i++) {
            const WlanStationAp* ap = wlan_station_map_ap_at(s_map, i);
            const char* vendor = wlan_oui_lookup(s_oui, ap->bssid);
            for(uint8_t j = 0; j < ap->client_count; j++) {
                const WlanStationClient* cli = &ap->clients[j];
                char row[128];
                int rn = snprintf(
                    row, sizeof(row),
                    "%02X:%02X:%02X:%02X:%02X:%02X,%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%u\n",
                    ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4],
                    ap->bssid[5], cli->mac[0], cli->mac[1], cli->mac[2], cli->mac[3], cli->mac[4],
                    cli->mac[5], vendor ? vendor : "Unknown", cli->rssi, cli->last_seen_tick);
                if(rn > 0) storage_file_write(f, row, (size_t)rn);
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    char detail[48];
    snprintf(detail, sizeof(detail), "%u APs %u clients", aps, station_total_clients());
    wlan_loot_record("stations", "station_scanner", detail);
}

void wlan_app_scene_station_scanner_on_enter(void* context) {
    WlanApp* app = context;
    s_map = malloc(sizeof(*s_map));
    s_ring = malloc(sizeof(*s_ring) * STATION_RX_RING);
    if(!s_map || !s_ring) {
        free(s_map); s_map = NULL;
        free(s_ring); s_ring = NULL;
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    s_head = s_tail = 0;
    wlan_station_map_reset(s_map);
    s_cur_ch = STATION_MIN_CH;
    s_hop_tick = 0;
    s_running = true;
    s_oui = wlan_oui_load(); /* NULL-tolerant */
    station_refresh_view(app);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, station_cb);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewStations);
}

bool wlan_app_scene_station_scanner_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running || !s_ring || !s_map)
        return false;

    /* Drain the RX ring into the map. */
    while(s_tail != s_head) {
        WlanStationRxEvt* e = &s_ring[s_tail];
        s_tail = (uint8_t)((s_tail + 1) % STATION_RX_RING);
        wlan_station_map_add(s_map, e->bssid, e->client_mac, e->rssi);
    }

    if(++s_hop_tick >= STATION_TICKS_PER_HOP) {
        s_hop_tick = 0;
        wlan_station_map_tick(s_map); /* advance the seconds counter */
        s_cur_ch = (s_cur_ch >= STATION_MAX_CH) ? STATION_MIN_CH : (uint8_t)(s_cur_ch + 1);
        wlan_hal_set_channel(s_cur_ch);
    }
    station_refresh_view(app);
    return false;
}

void wlan_app_scene_station_scanner_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL); /* stops HAL-worker delivery, NOT a
                                              WiFi-RX-task callback mid-flight */
    s_running = false;
    station_write_csv(); /* still needs s_map (GUI-thread-only) */
    if(s_oui) wlan_oui_free(s_oui);
    s_oui = NULL;
    /* NULL s_ring under the mux: a callback that acquires the mux after this sees
     * NULL and skips; one already inside completes before we free. */
    WlanStationRxEvt* ring_to_free;
    portENTER_CRITICAL(&s_station_ring_mux);
    ring_to_free = s_ring;
    s_ring = NULL;
    portEXIT_CRITICAL(&s_station_ring_mux);
    if(ring_to_free) free(ring_to_free);
    free(s_map); s_map = NULL;
}
