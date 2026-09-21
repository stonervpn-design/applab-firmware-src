#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include "../wlan_attack_detect.h"
#include "../wlan_probe_parse.h"
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define DET_TICK_MS 250
#define DET_TICKS_PER_SEC (1000 / DET_TICK_MS)
#define DET_MIN_CH 1
#define DET_MAX_CH 13

// Lock-free RX → scene handoff: a small event ring the cb fills, tick drains.
#define DET_RX_RING 32
typedef struct { uint8_t kind; uint8_t src[6]; char ssid[WLAN_PROBE_SSID_MAX]; uint8_t bssid[6]; } DetRxEvt;
// On-demand (was ~1.5 KB always-resident BSS). Written by det_cb on the WiFi
// task; allocated on enter before the cb registers, freed on exit after it
// unregisters.
static DetRxEvt* s_rx;
static volatile uint8_t s_rx_head, s_rx_tail;
static volatile uint32_t s_deauth_window; // deauth frames this second
static uint32_t s_deauth_per_sec, s_pwn_total, s_et_total;
static WlanEvilTwinTracker s_et;
static uint8_t s_cur_ch;
static bool s_running;
static uint16_t s_tick;
// Guards s_rx against a concurrent free() in on_exit while the WiFi RX task is
// inside det_cb. Same pattern as scene_pmkid.c (p5-concurrency-hardening):
// wlan_hal_set_promiscuous(false) syncs the HAL worker, NOT the WiFi RX task.
// (s_deauth_window is a never-freed static, so it needs no guard.)
static portMUX_TYPE s_det_rx_mux = portMUX_INITIALIZER_UNLOCKED;

static void det_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_rx) return; // buffer freed / not yet allocated
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    uint8_t src[6];
    WlanAlertType a = wlan_attack_classify(pkt->payload, len, src);
    if(a == WlanAlertDeauth) s_deauth_window++;
    // For evil-twin we need SSID+BSSID from beacons — push to ring for tick parse.
    // Publish into the ring under the mux so on_exit can't free s_rx out from
    // under a callback that already passed the fast NULL check above. portEXIT
    // is the release barrier ordering the payload stores before the head bump.
    portENTER_CRITICAL(&s_det_rx_mux);
    if(s_rx) { // authoritative re-check under the lock
        uint8_t nh = (uint8_t)((s_rx_head + 1) % DET_RX_RING);
        if(nh != s_rx_tail) { // ring not full
            DetRxEvt* e = &s_rx[s_rx_head];
            e->kind = (uint8_t)a;
            memcpy(e->src, src, 6);
            e->ssid[0] = '\0';
            if((pkt->payload[0] & 0xF0) == 0x80 && len >= 36) { // beacon
                memcpy(e->bssid, &pkt->payload[16], 6); // addr3 = BSSID
                wlan_probe_ssid_ie(pkt->payload, len, 36, e->ssid);
            }
            s_rx_head = nh;
        }
    }
    portEXIT_CRITICAL(&s_det_rx_mux);
}

void wlan_app_scene_attack_detector_on_enter(void* context) {
    WlanApp* app = context;
    s_rx = malloc(sizeof(*s_rx) * DET_RX_RING);
    if(!s_rx) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    s_rx_head = s_rx_tail = 0;
    s_deauth_window = 0;
    s_deauth_per_sec = s_pwn_total = s_et_total = 0;
    wlan_evil_twin_reset(&s_et);
    s_cur_ch = DET_MIN_CH;
    s_tick = 0;
    s_running = true;

    wlan_detector_view_set_counters(app->view_detector, 0, 0, 0);
    with_view_model(app->view_detector, WlanDetectorViewModel * m,
                    { m->alert_head = 0; m->alert_count = 0; }, true);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, det_cb);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewDetector);
}

bool wlan_app_scene_attack_detector_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running || !s_rx) return false;

    // Drain RX ring: evil-twin detection + pwnagotchi count + alert list.
    while(s_rx_tail != s_rx_head) {
        DetRxEvt* e = &s_rx[s_rx_tail];
        s_rx_tail = (uint8_t)((s_rx_tail + 1) % DET_RX_RING);
        char line[32];
        if(e->kind == WlanAlertPwnagotchi) {
            s_pwn_total++;
            snprintf(line, sizeof(line), "PWNGCH %02X%02X", e->src[4], e->src[5]);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "pwnagotchi", line);
        }
        if(e->ssid[0] && wlan_evil_twin_observe(&s_et, e->ssid, e->bssid)) {
            s_et_total++;
            snprintf(line, sizeof(line), "TWIN %.12s", e->ssid);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "evil_twin", e->ssid);
        }
    }

    // Deauth rate: fold the per-second window each full second.
    s_tick++;
    if(s_tick % DET_TICKS_PER_SEC == 0) {
        s_deauth_per_sec = s_deauth_window;
        s_deauth_window = 0;
        if(s_deauth_per_sec >= 10) { // flood threshold
            char line[32];
            snprintf(line, sizeof(line), "DEAUTH %lu/s", (unsigned long)s_deauth_per_sec);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "deauth_flood", line);
        }
    }
    wlan_detector_view_set_counters(app->view_detector, s_deauth_per_sec, s_pwn_total, s_et_total);

    // Channel hop each tick.
    s_cur_ch = (s_cur_ch >= DET_MAX_CH) ? DET_MIN_CH : (uint8_t)(s_cur_ch + 1);
    wlan_hal_set_channel(s_cur_ch);
    return false; // ticks are not "consumed"
}

void wlan_app_scene_attack_detector_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL); // stops HAL-worker delivery, NOT a
                                           // WiFi-RX-task callback mid-flight
    s_running = false;
    // NULL s_rx under the mux: a callback that acquires the mux after this sees
    // NULL and skips; one already inside completes before we free.
    DetRxEvt* rx_to_free;
    portENTER_CRITICAL(&s_det_rx_mux);
    rx_to_free = s_rx;
    s_rx = NULL;
    portEXIT_CRITICAL(&s_det_rx_mux);
    if(rx_to_free) free(rx_to_free);
}
