#include "../defender_app_i.h"
#include "../defender_deauth.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * WiFi Deauth/Disassoc flood detector scene.
 *
 * Radio discipline: this scene releases BT and drives the WiFi promiscuous tap
 * (defender_wifi_tap_*). Because wlan_hal_release_bt() requires a reboot to
 * restore BLE, on_enter shows a one-time (per app session) warning before the
 * radio is started. on_exit always stops the tap.
 *
 * Channel hopping is done inside the scene-manager tick (single-threaded with
 * the drain) rather than a separate FuriTimer, so no cross-thread HAL calls and
 * no timer lifecycle to leak. Hopping pauses (pins) once an alert fires so the
 * per-second rate stays accurate on the offending channel.
 */

#define DEAUTH_WARN_CONTINUE 200u
#define DEAUTH_WARN_BACK     201u
#define DEAUTH_OK_LOCATE     202u

#define DEAUTH_WIN_MAX  64u
#define DEAUTH_HOP_TICKS 5u /* tick=100ms → hop every ~500ms */
#define DEAUTH_UNPIN_TICKS 30u /* ~3s no-alert countdown to resume hopping */

static DefenderApp* s_app;
static bool s_warned;        /* persists across scene re-entries this session */
static bool s_monitoring;    /* false while the warning is displayed */
static uint8_t s_channel;
static bool s_pinned;
static uint32_t s_hop_ctr;
// No-PSRAM RAM diet: the 6 KB deauth record window is heap-allocated only while
// monitoring is active (alloc in start_monitoring, freed in on_exit) instead of
// sitting in .bss. All access is on the GUI thread (tick handler drains the tap
// into it), so a plain malloc/free with NULL-guards is safe.
static DefenderDeauthRec* s_win;
static uint32_t s_win_count;
static uint8_t s_no_alert_ticks;  /* countdown to unpin after alert stops */

static bool defender_deauth_ok_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(s_app->view_dispatcher, DEAUTH_OK_LOCATE);
        return true;
    }
    return false;
}

static void defender_deauth_warn_cb(void* context, uint32_t index) {
    DefenderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

// Drop records older than the window, compacting s_win in place.
static void defender_deauth_evict_stale(uint32_t now_ms) {
    if(!s_win) {
        s_win_count = 0;
        return;
    }
    uint32_t kept = 0;
    for(uint32_t i = 0; i < s_win_count; i++) {
        if((uint32_t)(now_ms - s_win[i].ts_ms) < DEAUTH_WINDOW_MS) {
            if(kept != i) s_win[kept] = s_win[i];
            kept++;
        }
    }
    s_win_count = kept;
}

static void defender_deauth_append(const DefenderDeauthRec* rec) {
    if(!s_win) return;
    if(s_win_count >= DEAUTH_WIN_MAX) {
        // Drop oldest to make room (shift down by one).
        memmove(&s_win[0], &s_win[1], (DEAUTH_WIN_MAX - 1) * sizeof(DefenderDeauthRec));
        s_win_count = DEAUTH_WIN_MAX - 1;
    }
    s_win[s_win_count++] = *rec;
}

static void defender_deauth_start_monitoring(DefenderApp* app) {
    // Alloc the 6 KB record window for the monitoring session (freed in on_exit).
    // If the heap can't spare it, monitoring still runs (rate meter counts from the
    // tap drain); the record window just stays empty via the NULL-guards above.
    if(!s_win) s_win = malloc(sizeof(DefenderDeauthRec) * DEAUTH_WIN_MAX);
    s_monitoring = true;
    s_channel = 6;
    s_pinned = false;
    s_hop_ctr = 0;
    s_win_count = 0;
    s_no_alert_ticks = 0;

    // Reset the rate-meter view and route its OK to the locator handoff.
    DefenderRatemeterModel* m = view_get_model(app->ratemeter);
    memset(m, 0, sizeof(DefenderRatemeterModel));
    snprintf(m->title, sizeof(m->title), "Deauth ch%u", s_channel);
    view_commit_model(app->ratemeter, true);
    view_set_input_callback(app->ratemeter, defender_deauth_ok_input);

    defender_wifi_tap_start(s_channel);
    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewRateMeter);
}

void defender_scene_deauth_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    s_app = app;

    if(!s_warned) {
        s_monitoring = false;
        submenu_reset(app->submenu);
        submenu_set_header(app->submenu, "WiFi disables BLE");
        submenu_add_item(
            app->submenu, "Continue", DEAUTH_WARN_CONTINUE, defender_deauth_warn_cb, app);
        submenu_add_item(app->submenu, "Back", DEAUTH_WARN_BACK, defender_deauth_warn_cb, app);
        view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewSubmenu);
        return;
    }

    defender_deauth_start_monitoring(app);
}

bool defender_scene_deauth_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DEAUTH_WARN_CONTINUE) {
            s_warned = true;
            defender_deauth_start_monitoring(app);
            consumed = true;
        } else if(event.event == DEAUTH_WARN_BACK) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == DEAUTH_OK_LOCATE) {
            // Use the last-known top offender captured in the view model.
            DefenderRatemeterModel* m = view_get_model(app->ratemeter);
            snprintf(
                app->locator_label,
                sizeof(app->locator_label),
                "AP %02X:%02X:%02X",
                m->top_bssid[3],
                m->top_bssid[4],
                m->top_bssid[5]);
            view_commit_model(app->ratemeter, false);
            app->locator_floor = -90;
            app->locator_ceil = -40;
            app->locator_active = true;
            scene_manager_next_scene(app->scene_manager, DefenderSceneLocator);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick && s_monitoring) {
        uint32_t now = furi_get_tick();

        // Channel hop (paused while pinned on an active offender).
        if(!s_pinned && (++s_hop_ctr % DEAUTH_HOP_TICKS) == 0) {
            s_channel = (s_channel >= 13) ? 1 : (uint8_t)(s_channel + 1);
            defender_wifi_tap_set_channel(s_channel);
        }

        // Drain new captures into the sliding window.
        DefTapDeauthRec tmp[32];
        uint32_t n = defender_wifi_tap_drain_deauth(tmp, 32);
        defender_deauth_evict_stale(now);
        for(uint32_t i = 0; i < n; i++) {
            DefenderDeauthRec r;
            memcpy(r.bssid, tmp[i].bssid, 6);
            r.broadcast = tmp[i].broadcast;
            r.rssi = tmp[i].rssi;
            r.ts_ms = tmp[i].ts_ms;
            defender_deauth_append(&r);
        }

        DefenderDeauthResult res;
        defender_deauth_eval(s_win, s_win_count, now, DEAUTH_WINDOW_MS, DEAUTH_RATE_ALERT, &res);

        DefenderRatemeterModel* m = view_get_model(app->ratemeter);
        snprintf(m->title, sizeof(m->title), "Deauth ch%u", s_channel);
        m->rate = res.weighted_count;
        memcpy(m->top_bssid, res.top_bssid, 6);
        m->top_count = res.top_count;
        m->top_rssi = res.top_rssi;
        m->alert = res.alert;
        view_commit_model(app->ratemeter, true);

        if(res.alert) {
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatWifiDeauth;
            a.severity = 2;
            memcpy(a.offender, res.top_bssid, 6);
            a.rssi = res.top_rssi;
            a.ts_ms = now;
            snprintf(
                a.msg,
                sizeof(a.msg),
                "Deauth flood ch%u %lu/s",
                s_channel,
                (unsigned long)res.weighted_count);
            defender_alert_raise(&app->alert, &a); // debounced inside the bus
            s_pinned = true;                        // stay on this channel
            s_no_alert_ticks = 0;                   // reset no-alert countdown
        } else if(s_pinned) {
            // Pinned but no alert: countdown to unpin.
            if(++s_no_alert_ticks >= DEAUTH_UNPIN_TICKS) {
                s_pinned = false;  // Resume hopping.
                s_no_alert_ticks = 0;
            }
        }
        consumed = true;
    }

    return consumed;
}

void defender_scene_deauth_on_exit(void* context) {
    furi_assert(context);
    UNUSED(context);

    if(s_monitoring) {
        defender_wifi_tap_stop(); // clears promiscuous + stops WiFi
        s_monitoring = false;
    }
    s_pinned = false;
    s_win_count = 0;
    if(s_win) {
        free(s_win);
        s_win = NULL;
    }
    furi_hal_light_blink_stop();  // Belt-and-suspenders: clear any lingering blink on scene exit.
}
