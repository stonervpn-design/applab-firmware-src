#include "../wlan_app.h"

// PRIVATE-only: passive WPS ("Wi-Fi Protected Setup") recon scene (WF2 Task
// 2). Promiscuously hops channels 1..13 listening for beacon/probe-response
// mgmt frames, feeds each captured frame's tagged-IE region to Task 1's
// wlan_wps_parse_ies(), and lists nearby APs that advertise WPS along with
// their posture (locked / config-methods). PASSIVE RECEIVE + PARSE ONLY --
// no transmit, no association, no PIN/Pixie-Dust attempt (that attack is
// explicitly off-device; this screen is recon only).
//
// Structurally cloned from scene_station_scanner.c: promiscuous cb -> mux-
// guarded SPSC ring (WiFi task writer) -> Tick drains the ring on the GUI
// thread -> channel hop every N ticks -> on_exit demotes promiscuous mode.
// Differs from that clone in what the ring carries: station_scanner needs
// only the 12-byte station pairing, so it does all its work in the cb. Here
// the heavier step -- IE-region slicing + wlan_wps_parse_ies() + SSID
// extraction -- happens at Tick time instead, so the cb just copies the raw
// captured frame (bounded to WPS_FRAME_CAP bytes) into the ring.
//
// No dedicated View exists for this screen (adding one would mean touching
// wlan_app.h/.c, outside this task's file list) -- like scene_mgmt_fuzzer_run
// it reuses app->widget, rebuilt via widget_add_text_scroll_element() on
// every Tick from app->text_buf (the same shared scratch FuriString
// scene_loot_preview.c uses), exactly like nrf24's scene_keysniffer.c
// live-log idiom.
//
// No ADD_SCENE reference to this file exists in a PUBLIC_BUILD compile
// (scene_config.h gates it), so this whole body is guarded too, adding 0
// symbols there -- mirrors WF1's scene_mgmt_fuzzer_run.c gating exactly.
#ifndef PUBLIC_BUILD

#include "../wlan_hal.h"
#include "../wlan_probe_parse.h"
#include "../helpers/wlan_wps_parse.h"
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define WPS_MIN_CH 1
#define WPS_MAX_CH 13
#define WPS_TICKS_PER_HOP 4 /* ~1s dwell per channel at the 250ms scene tick */

// RX callback -> Tick consumer, lock-free SPSC ring of raw captured frames.
// Each slot copies up to WPS_FRAME_CAP bytes of the frame (24-byte 802.11
// header + 12-byte fixed beacon/probe-resp params + tagged IEs); a longer
// frame is safely truncated by the copy (wlan_wps_parse_ies bounds-checks
// its input), only risking a missed WPS IE placed unusually late in an
// unusually IE-heavy beacon -- not a correctness/safety issue.
#define WPS_RX_RING 12
#define WPS_FRAME_CAP 320
typedef struct {
    uint8_t data[WPS_FRAME_CAP];
    uint16_t len; /* actual captured bytes, clipped to WPS_FRAME_CAP */
    uint8_t channel; /* channel this scene was tuned to when captured */
} WlanWpsRxEvt;

// Bounded, BSSID-keyed AP list built from parsed frames.
#define WPS_AP_CAP 32
typedef struct {
    uint8_t bssid[6];
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t channel;
    bool locked;
    uint16_t config_methods;
} WlanWpsApRow;

// On-demand (station_scanner precedent): allocated on enter before the cb
// registers, freed on exit after it unregisters. s_ring is written by wps_cb
// on the WiFi task; s_aps is GUI-thread-only (Tick).
static WlanWpsRxEvt* s_ring;
static volatile uint8_t s_head, s_tail;
static WlanWpsApRow* s_aps;
static uint8_t s_ap_count;
static volatile uint8_t s_cur_ch;
static uint8_t s_hop_tick;
static bool s_running;
/* Guards s_ring against a concurrent free() in on_exit while the WiFi RX task
 * is inside wps_cb. Same pattern as scene_station_scanner.c/scene_pmkid.c
 * (p5-concurrency-hardening): wlan_hal_set_promiscuous(false) syncs the HAL
 * worker, NOT the WiFi RX task. */
static portMUX_TYPE s_wps_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void wps_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_ring) return; /* buffer freed / not yet allocated */
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    /* Need at least the 24-byte header + 12-byte fixed params to have any IE
     * region at all -- shorter frames can't carry a WPS IE, skip early. */
    if(len < 36) return;

    /* Frame-control byte0: version(2) | type(2)<<2 | subtype(4)<<4 -- same
     * convention as wlan_probe_extract()'s comment in wlan_probe_parse.c.
     * Beacon: type=00, subtype=1000 -> 0x80. Probe-response: type=00,
     * subtype=0101 -> 0x50. type==WIFI_PKT_MGMT was already filtered by the
     * HAL above, so masking just the subtype nibble is sufficient here. */
    uint8_t subtype = pkt->payload[0] & 0xF0;
    if(subtype != 0x80 && subtype != 0x50) return; /* only beacon / probe-resp */

    /* Publish into the ring under the mux so on_exit can't free s_ring out
     * from under a callback that already passed the fast NULL check above.
     * portEXIT is the release barrier ordering payload stores before the
     * head bump. */
    portENTER_CRITICAL(&s_wps_ring_mux);
    if(s_ring) { /* authoritative re-check under the lock */
        uint8_t nh = (uint8_t)((s_head + 1) % WPS_RX_RING);
        if(nh != s_tail) { /* ring not full */
            uint16_t copy_len = len > WPS_FRAME_CAP ? WPS_FRAME_CAP : len;
            memcpy(s_ring[s_head].data, pkt->payload, copy_len);
            s_ring[s_head].len = copy_len;
            s_ring[s_head].channel = s_cur_ch;
            s_head = nh;
        }
    }
    portEXIT_CRITICAL(&s_wps_ring_mux);
}

static void wps_ap_upsert(
    const uint8_t bssid[6], const char* ssid, uint8_t channel, bool locked,
    uint16_t config_methods) {
    if(!s_aps) return;
    for(uint8_t i = 0; i < s_ap_count; i++) {
        if(memcmp(s_aps[i].bssid, bssid, 6) == 0) {
            s_aps[i].channel = channel;
            s_aps[i].locked = locked;
            s_aps[i].config_methods = config_methods;
            if(ssid[0]) { /* keep the prior SSID if this frame had none (e.g. hidden probe-resp) */
                strncpy(s_aps[i].ssid, ssid, WLAN_PROBE_SSID_MAX - 1);
                s_aps[i].ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
            }
            return;
        }
    }
    if(s_ap_count >= WPS_AP_CAP) return; /* bounded list: cap reached, drop new APs */
    WlanWpsApRow* row = &s_aps[s_ap_count++];
    memcpy(row->bssid, bssid, 6);
    strncpy(row->ssid, ssid[0] ? ssid : "(hidden)", WLAN_PROBE_SSID_MAX - 1);
    row->ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
    row->channel = channel;
    row->locked = locked;
    row->config_methods = config_methods;
}

// Compute the IE region (skip 24-byte 802.11 header + 12-byte fixed
// beacon/probe-resp params -- both frame types share this 36-byte fixed
// prefix, only the tagged params after it differ per-AP), parse it for a WPS
// IE, and upsert the AP row if present. Non-WPS APs are not tracked (spec:
// list APs that *advertise* WPS).
static void wps_process_frame(const WlanWpsRxEvt* e) {
    if(e->len < 36) return;
    const uint8_t* frame = e->data;
    uint16_t len = e->len;
    const uint8_t* bssid = &frame[16]; /* addr3: BSSID for beacon/probe-resp */
    const uint8_t* ies = frame + 36;
    uint16_t ies_len = (uint16_t)(len - 36);

    WlanWpsInfo info;
    if(!wlan_wps_parse_ies(ies, ies_len, &info)) return; /* no WPS IE in this frame */

    char ssid[WLAN_PROBE_SSID_MAX];
    wlan_probe_ssid_ie(ies, ies_len, 0, ssid); /* reuses the probe-sniffer SSID-IE walk; leaves "" if none/hidden */

    wps_ap_upsert(bssid, ssid, e->channel, info.locked, info.config_methods);
}

// Short config-methods hint for the row: PBC (push-button), PIN (any of
// Label/Display/Keypad -- all imply a static or displayed PIN entry path),
// both, or "?" if WPS is present but neither common method bit is set.
static const char* wps_methods_hint(uint16_t cm) {
    bool pbc = (cm & WLAN_WPS_CM_PUSHBUTTON) != 0;
    bool pin = (cm & (WLAN_WPS_CM_KEYPAD | WLAN_WPS_CM_LABEL | WLAN_WPS_CM_DISPLAY)) != 0;
    if(pbc && pin) return "PBC+PIN";
    if(pbc) return "PBC";
    if(pin) return "PIN";
    return "?";
}

static void wps_refresh_view(WlanApp* app) {
    furi_string_reset(app->text_buf);
    // Honest-UI header: states this is passive recon, the actual PIN /
    // Pixie-Dust attack is off-device -- per spec, never faked here.
    furi_string_cat_printf(
        app->text_buf,
        "\e#WPS Scan (passive)\e#\nch %u  APs:%u/%u\nRecon only -- PIN/Pixie attack is off-device\n\n",
        (unsigned)s_cur_ch,
        (unsigned)s_ap_count,
        (unsigned)WPS_AP_CAP);

    for(uint8_t i = 0; i < s_ap_count; i++) {
        WlanWpsApRow* ap = &s_aps[i];
        furi_string_cat_printf(
            app->text_buf,
            "%.14s ch%u WPS:ON%s %s\n",
            ap->ssid,
            (unsigned)ap->channel,
            ap->locked ? "(LOCK)" : "",
            wps_methods_hint(ap->config_methods));
    }
    if(s_ap_count == 0) {
        furi_string_cat_printf(app->text_buf, "(no WPS APs seen yet)\n");
    }

    widget_reset(app->widget);
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 64, furi_string_get_cstr(app->text_buf));
}

void wlan_app_scene_wps_scan_on_enter(void* context) {
    WlanApp* app = context;
    s_aps = malloc(sizeof(*s_aps) * WPS_AP_CAP);
    s_ring = malloc(sizeof(*s_ring) * WPS_RX_RING);
    if(!s_aps || !s_ring) {
        free(s_aps); s_aps = NULL;
        free(s_ring); s_ring = NULL;
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    s_head = s_tail = 0;
    s_ap_count = 0;
    s_cur_ch = WPS_MIN_CH;
    s_hop_tick = 0;
    s_running = true;
    wps_refresh_view(app);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, wps_cb);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_wps_scan_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running || !s_ring || !s_aps)
        return false;

    /* Drain the RX ring, parsing + upserting each captured frame. */
    while(s_tail != s_head) {
        WlanWpsRxEvt* e = &s_ring[s_tail];
        wps_process_frame(e);
        s_tail = (uint8_t)((s_tail + 1) % WPS_RX_RING);
    }

    if(++s_hop_tick >= WPS_TICKS_PER_HOP) {
        s_hop_tick = 0;
        s_cur_ch = (s_cur_ch >= WPS_MAX_CH) ? WPS_MIN_CH : (uint8_t)(s_cur_ch + 1);
        wlan_hal_set_channel(s_cur_ch);
    }
    wps_refresh_view(app);
    return false;
}

void wlan_app_scene_wps_scan_on_exit(void* context) {
    WlanApp* app = context;
    wlan_hal_set_promiscuous(false, NULL); /* stops HAL-worker delivery, NOT a
                                              WiFi-RX-task callback mid-flight */
    s_running = false;
    widget_reset(app->widget);
    /* NULL s_ring under the mux: a callback that acquires the mux after this
     * sees NULL and skips; one already inside completes before we free. */
    WlanWpsRxEvt* ring_to_free;
    portENTER_CRITICAL(&s_wps_ring_mux);
    ring_to_free = s_ring;
    s_ring = NULL;
    portEXIT_CRITICAL(&s_wps_ring_mux);
    if(ring_to_free) free(ring_to_free);
    free(s_aps); s_aps = NULL;
    s_ap_count = 0;
}

#endif // PUBLIC_BUILD
