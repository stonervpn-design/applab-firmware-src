#include "../wlan_app.h"

// PRIVATE-only: 802.11 Mgmt Fuzzer run screen + worker (WF1 Task 3). Worker
// lifecycle (xTaskCreate + volatile stop-flag + blocking join) is cloned
// straight from scene_network_deauth.c's deauth_task_fn/_worker_start/_stop
// shape. UI is a Widget (not the WlanAppViewDeauther/WlanDeautherModel pair --
// that model's title[24] can't hold the mandated disclaimer string and its
// fields are deauth-specific), rebuilt on toggle + Tick like
// scene_ssid_spam_run.c's widget_reset()+rebuild idiom. No ADD_SCENE
// reference to this file exists in a PUBLIC_BUILD compile (scene_config.h
// gates it), so this whole body is also guarded to add 0 symbols there.
#ifndef PUBLIC_BUILD

#include "../wlan_hal.h"
#include "../pmkid_frames.h"
#include "../helpers/wlan_mgmt_fuzz.h"
#include "../helpers/wlan_mgmt_frames.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define MGMT_FUZZER_TAG "WlanMgmtFuzzer"
#define MGMT_FUZZER_TX_PERIOD_MS 15
#define MGMT_FUZZER_BASE_CAP 128

// Local custom-event id for the OK/toggle button. Scoped to this scene only
// (no shared views/wlan_view_events.h edit) -- mirrors scene_ssid_spam.c's
// local-enum-as-custom-event idiom. Picked well above the WlanAppCustomEvent
// range (100-202) used by that header so it can never collide.
#define MGMT_FUZZER_EVENT_TOGGLE 900

// wlan_hal_send_raw() enforces a 128-byte TX ceiling (esp_wifi_80211_tx). The
// mutator can emit up to WLAN_FUZZ_MAX (256); we truncate to this before TX so
// every mutated frame actually reaches the air (a truncated malformed frame is
// still a valid fuzz input) instead of being silently dropped.
#define MGMT_FUZZER_TX_MAX 128

// Frame-type index selected in scene_mgmt_fuzzer_config.c. Kept in sync by
// hand with that file's MgmtFuzzerFrameType enum (no shared header exists
// for this pair of files -- see that file's top-of-file comment).
extern uint8_t wlan_mgmt_fuzzer_frame_type;
extern uint8_t wlan_mgmt_fuzzer_mode;

static const char* const s_frame_type_names[] = {"Auth", "Assoc", "Probe", "Beacon", "Deauth"};
static const char* const s_fuzz_mode_names[] =
    {"BitFlip", "Field", "IE Mangle", "LenSweep", "Random"};

// Worker-State (xTaskCreate, NICHT FuriThread -- darf wlan_hal_* blockierend
// aufrufen, exakt wie scene_network_deauth.c's deauth_task_fn).
static volatile TaskHandle_t s_task = NULL;
static volatile bool s_task_run = false;
static volatile uint32_t s_frames_sent = 0;
static volatile uint32_t s_step = 0;
static uint8_t s_frame_type_snap = 0;
static uint8_t s_mode_snap = 0;
static const char* s_err = NULL; // non-NULL => last start attempt failed (shown in UI)

static void mgmt_fuzz_task_fn(void* arg) {
    WlanApp* app = arg;
    uint8_t base[MGMT_FUZZER_BASE_CAP];
    uint16_t base_len = 0;
    uint8_t out[WLAN_FUZZ_MAX];
    uint16_t out_len = 0;

    // Target resolution: app->target_ap (Attack flow) > connected AP >
    // broadcast + channel 1. Mirrors deauth_apply_target()'s src precedence
    // in scene_network_deauth.c.
    uint8_t bssid[6];
    uint8_t channel;
    char ssid_buf[33] = {0};
    if(app->target_ap.ssid[0]) {
        memcpy(bssid, app->target_ap.bssid, 6);
        channel = app->target_ap.channel ? app->target_ap.channel : 1;
        strncpy(ssid_buf, app->target_ap.ssid, sizeof(ssid_buf) - 1);
    } else if(app->connected) {
        memcpy(bssid, app->connected_ap.bssid, 6);
        channel = app->connected_ap.channel ? app->connected_ap.channel : 1;
        strncpy(ssid_buf, app->connected_ap.ssid, sizeof(ssid_buf) - 1);
    } else {
        memset(bssid, 0xff, 6);
        channel = 1;
    }

    // Locally-administered source MAC (bit1 set, bit0 clear = unicast) so the
    // device's real factory MAC never goes on the air -- same rationale as
    // wlan_hal_random_mac(), fixed here (not randomized) so a capture of the
    // run is trivially attributable to this tool by a defender.
    static const uint8_t src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

    wlan_hal_set_channel(channel);
    uint32_t seed = furi_get_tick();

    while(s_task_run) {
        switch(s_frame_type_snap) {
        case 0: // Auth
            base_len = (uint16_t)pmkid_build_auth_open(base, sizeof(base), bssid, src_mac);
            break;
        case 1: // Assoc
            base_len =
                (uint16_t)pmkid_build_assoc_req(base, sizeof(base), bssid, src_mac, ssid_buf);
            break;
        case 2: // Probe
            base_len = wlan_build_probe_req(base, sizeof(base), src_mac, ssid_buf);
            break;
        case 3: // Beacon
            base_len = wlan_build_beacon(base, sizeof(base), bssid, ssid_buf, channel);
            break;
        case 4: // Deauth
        default:
            base_len = wlan_build_deauth(base, sizeof(base), bssid, src_mac, 7);
            break;
        }

        if(base_len >= 24 &&
           wlan_mgmt_fuzz_next(
               base, base_len, (WlanFuzzMode)s_mode_snap, seed, s_step, out, &out_len)) {
            // Truncate to the 128-byte TX ceiling so LenSweep/Random steps that
            // mutate up to WLAN_FUZZ_MAX (256) still transmit (see the TX_MAX
            // comment above); s_frames_sent counts only frames actually queued.
            if(out_len > MGMT_FUZZER_TX_MAX) out_len = MGMT_FUZZER_TX_MAX;
            if(wlan_hal_send_raw(out, out_len)) s_frames_sent++;
        }
        s_step++;

        vTaskDelay(pdMS_TO_TICKS(MGMT_FUZZER_TX_PERIOD_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

static bool mgmt_fuzzer_worker_start(WlanApp* app) {
    if(s_task_run || s_task) return true; // already running

    if(!wlan_hal_is_started()) {
        wlan_hal_hint_skip_netif(); // TX-only feature: skip IP stack (frees DMA)
        if(!wlan_hal_start()) return false;
    }

    s_frame_type_snap = wlan_mgmt_fuzzer_frame_type;
    s_mode_snap = wlan_mgmt_fuzzer_mode;
    s_frames_sent = 0;
    s_step = 0;
    s_task_run = true;
    BaseType_t rc = xTaskCreate(
        mgmt_fuzz_task_fn, "WlanMgmtFuzzer", 4096, app, 5, (TaskHandle_t*)&s_task);
    if(rc != pdPASS) {
        s_task_run = false;
        s_task = NULL;
        ESP_LOGE(MGMT_FUZZER_TAG, "xTaskCreate failed");
        return false;
    }
    ESP_LOGI(
        MGMT_FUZZER_TAG,
        "worker started (frame_type=%u mode=%u)",
        (unsigned)s_frame_type_snap,
        (unsigned)s_mode_snap);
    return true;
}

// Task beendet sich selbst (vTaskDelete im Task); hier nur auf Beendigung
// warten -- exakt deauth_worker_stop()'s Muster.
static void mgmt_fuzzer_worker_stop(void) {
    if(!s_task_run && !s_task) return;
    s_task_run = false;
    while(s_task) vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(MGMT_FUZZER_TAG, "worker stopped, frames=%lu", (unsigned long)s_frames_sent);
}

static void mgmt_fuzzer_run_rebuild(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 6, AlignCenter, AlignTop, FontPrimary, "Mgmt Fuzzer");

    char cfg_buf[32];
    snprintf(
        cfg_buf,
        sizeof(cfg_buf),
        "%s / %s",
        s_frame_type_names[s_frame_type_snap],
        s_fuzz_mode_names[s_mode_snap]);
    widget_add_string_element(app->widget, 64, 18, AlignCenter, AlignTop, FontSecondary, cfg_buf);

    char counters_buf[40];
    snprintf(
        counters_buf,
        sizeof(counters_buf),
        "Sent: %lu  Step: %lu",
        (unsigned long)s_frames_sent,
        (unsigned long)s_step);
    widget_add_string_element(
        app->widget, 64, 30, AlignCenter, AlignTop, FontSecondary, counters_buf);

    // On-air indicator, doubles as the OK-button hint. If the last start
    // attempt failed (HAL/OOM), surface that instead of "Stopped".
    const char* status = s_task_run  ? "* ON AIR *  (OK: Stop)" :
                         s_err        ? s_err :
                                        "Stopped  (OK: Start)";
    widget_add_string_element(app->widget, 64, 41, AlignCenter, AlignTop, FontSecondary, status);

    // Honest-UI disclaimer, verbatim per spec.
    widget_add_text_box_element(
        app->widget,
        0,
        50,
        128,
        14,
        AlignCenter,
        AlignTop,
        "Effect is target-dependent -- reports frames sent, not a crash verdict.",
        false);
}

static void mgmt_fuzzer_ok_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MGMT_FUZZER_EVENT_TOGGLE);
    }
}

void wlan_app_scene_mgmt_fuzzer_run_on_enter(void* context) {
    WlanApp* app = context;
    s_frame_type_snap = wlan_mgmt_fuzzer_frame_type;
    s_mode_snap = wlan_mgmt_fuzzer_mode;
    s_frames_sent = 0;
    s_step = 0;
    s_err = NULL;

    mgmt_fuzzer_run_rebuild(app);
    widget_add_button_element(app->widget, GuiButtonTypeCenter, "Start", mgmt_fuzzer_ok_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_mgmt_fuzzer_run_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MGMT_FUZZER_EVENT_TOGGLE) {
            // OK is a toggle: press once to start, press again to stop.
            if(s_task_run || s_task) {
                mgmt_fuzzer_worker_stop();
                s_err = NULL;
            } else {
                // Surface a start failure (radio won't come up / OOM) instead of
                // silently re-rendering "Start" -- mirrors deauth's status msg.
                s_err = mgmt_fuzzer_worker_start(app) ? NULL : "Radio start failed";
            }
            mgmt_fuzzer_run_rebuild(app);
            widget_add_button_element(
                app->widget,
                GuiButtonTypeCenter,
                s_task_run ? "Stop" : "Start",
                mgmt_fuzzer_ok_cb,
                app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Widget alle ~250 ms mit aktuellem Frame-Counter neu zeichnen.
        mgmt_fuzzer_run_rebuild(app);
        widget_add_button_element(
            app->widget,
            GuiButtonTypeCenter,
            s_task_run ? "Stop" : "Start",
            mgmt_fuzzer_ok_cb,
            app);
    }

    return consumed;
}

void wlan_app_scene_mgmt_fuzzer_run_on_exit(void* context) {
    WlanApp* app = context;
    // Stop + join the worker FIRST so it can never touch app-> fields after
    // the widget/view underneath it goes away (mirrors
    // deauth_worker_stop()/subghz_scene_protocol_fuzzer_run_on_exit).
    mgmt_fuzzer_worker_stop();
    widget_reset(app->widget);
}

#endif // PUBLIC_BUILD
