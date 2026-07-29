#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include "../wlan_probe_parse.h"
#include "../wlan_probe_table.h"
#include <storage/storage.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define PROBE_MIN_CH 1
#define PROBE_MAX_CH 13

// RX → tick ring of (mac, ssid).
#define PROBE_RX_RING 32
typedef struct { uint8_t mac[6]; char ssid[WLAN_PROBE_SSID_MAX]; } ProbeRxEvt;
// Allocated on scene enter, freed on exit after the promiscuous callback is
// unregistered (were ~1.2 KB + ~4.6 KB always-resident BSS). s_ring is written
// by probe_cb on the WiFi task; s_table is GUI-thread-only.
static ProbeRxEvt* s_ring;
static volatile uint8_t s_head, s_tail;
static WlanProbeTable* s_table;
static uint8_t s_cur_ch;
static bool s_running;
// Guards s_ring against a concurrent free() in on_exit while the WiFi RX task is
// inside probe_cb. Same pattern as scene_pmkid.c (p5-concurrency-hardening):
// wlan_hal_set_promiscuous(false) syncs the HAL worker, NOT the WiFi RX task.
static portMUX_TYPE s_probe_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void probe_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_ring) return; // buffer freed / not yet allocated
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t mac[6];
    if(!wlan_probe_extract(pkt->payload, len, ssid, mac)) return;
    // Publish into the ring under the mux so on_exit can't free s_ring out from
    // under a callback that already passed the fast NULL check above. portEXIT
    // is the release barrier ordering the payload stores before the head bump.
    portENTER_CRITICAL(&s_probe_ring_mux);
    if(s_ring) { // authoritative re-check under the lock
        uint8_t nh = (uint8_t)((s_head + 1) % PROBE_RX_RING);
        if(nh != s_tail) {
            memcpy(s_ring[s_head].mac, mac, 6);
            strncpy(s_ring[s_head].ssid, ssid, WLAN_PROBE_SSID_MAX - 1);
            s_ring[s_head].ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
            s_head = nh;
        }
    }
    portEXIT_CRITICAL(&s_probe_ring_mux);
}

// Build the display lines from the most-recently-touched table entries.
static void probe_refresh_view(WlanApp* app) {
    char lines[PROBE_VIEW_ROWS][32];
    uint8_t n = 0;
    // pick up to PROBE_VIEW_ROWS entries with the highest seq
    uint32_t used_seq[PROBE_VIEW_ROWS] = {0};
    int used_idx[PROBE_VIEW_ROWS];
    for(int i = 0; i < PROBE_VIEW_ROWS; i++) used_idx[i] = -1;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(!s_table->entries[i].used) continue;
        uint32_t seq = s_table->entries[i].seq;
        for(int r = 0; r < PROBE_VIEW_ROWS; r++) {
            if(used_idx[r] < 0 || seq > used_seq[r]) {
                for(int s = PROBE_VIEW_ROWS - 1; s > r; s--) {
                    used_seq[s] = used_seq[s - 1]; used_idx[s] = used_idx[s - 1];
                }
                used_seq[r] = seq; used_idx[r] = i;
                break;
            }
        }
    }
    for(int r = 0; r < PROBE_VIEW_ROWS; r++) {
        if(used_idx[r] < 0) break;
        WlanPtEntry* e = &s_table->entries[used_idx[r]];
        const char* ss = e->ssid_count ? e->ssids[e->ssid_count - 1] : "*";
        snprintf(lines[n], sizeof(lines[0]), "%02X%02X%02X %.16s",
                 e->mac[3], e->mac[4], e->mac[5], ss);
        n++;
    }
    wlan_probe_view_update(app->view_probe, lines, n,
                           wlan_probe_table_client_count(s_table),
                           wlan_probe_table_ssid_count(s_table));
}

static void probe_write_csv(void) {
    if(wlan_probe_table_client_count(s_table) == 0) return;
    const char* dir = wlan_loot_session_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/probes.csv", dir);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "mac,ssid\n";
        storage_file_write(f, hdr, strlen(hdr));
        for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
            WlanPtEntry* e = &s_table->entries[i];
            if(!e->used) continue;
            for(uint8_t j = 0; j < e->ssid_count; j++) {
                char row[64];
                int rn = snprintf(row, sizeof(row), "%02X:%02X:%02X:%02X:%02X:%02X,%.32s\n",
                                  e->mac[0], e->mac[1], e->mac[2],
                                  e->mac[3], e->mac[4], e->mac[5], e->ssids[j]);
                if(rn > 0) storage_file_write(f, row, (size_t)rn);
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    char detail[32];
    snprintf(detail, sizeof(detail), "%u clients", wlan_probe_table_client_count(s_table));
    wlan_loot_record("probes", "probe_sniffer", detail);
}

void wlan_app_scene_probe_sniffer_on_enter(void* context) {
    WlanApp* app = context;
    // Allocate before registering probe_cb (which writes s_ring on the WiFi task).
    s_table = malloc(sizeof(*s_table));
    s_ring = malloc(sizeof(*s_ring) * PROBE_RX_RING);
    if(!s_table || !s_ring) {
        free(s_table); s_table = NULL;
        free(s_ring); s_ring = NULL;
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    s_head = s_tail = 0;
    wlan_probe_table_reset(s_table);
    s_cur_ch = PROBE_MIN_CH;
    s_running = true;
    probe_refresh_view(app);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, probe_cb);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewProbe);
}

bool wlan_app_scene_probe_sniffer_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running || !s_ring || !s_table)
        return false;
    while(s_tail != s_head) {
        ProbeRxEvt* e = &s_ring[s_tail];
        s_tail = (uint8_t)((s_tail + 1) % PROBE_RX_RING);
        wlan_probe_table_add(s_table, e->mac, e->ssid);
    }
    probe_refresh_view(app);
    s_cur_ch = (s_cur_ch >= PROBE_MAX_CH) ? PROBE_MIN_CH : (uint8_t)(s_cur_ch + 1);
    wlan_hal_set_channel(s_cur_ch);
    return false;
}

void wlan_app_scene_probe_sniffer_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL); // stops HAL-worker delivery, NOT a
                                           // WiFi-RX-task callback mid-flight
    s_running = false;
    probe_write_csv(); // still needs s_table (GUI-thread-only)
    // NULL s_ring under the mux: a callback that acquires the mux after this
    // sees NULL and skips; one already inside completes before we free.
    ProbeRxEvt* ring_to_free;
    portENTER_CRITICAL(&s_probe_ring_mux);
    ring_to_free = s_ring;
    s_ring = NULL;
    portEXIT_CRITICAL(&s_probe_ring_mux);
    if(ring_to_free) free(ring_to_free);
    free(s_table); s_table = NULL;
}
