#include "../defender_app_i.h"
#include "../defender_rogue_ap.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <gui/canvas.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * Rogue-AP detector scene (blue team). Drives the WiFi promiscuous tap, drains
 * beacon frames into a sliding window, and runs the pure evil-twin / beacon-flood
 * evaluator. Passive: it never transmits. Same BT-disables warning + channel-hop
 * discipline as the deauth scene; on_exit always stops the tap.
 */

#define ROGUE_WARN_CONTINUE 210u
#define ROGUE_WARN_BACK 211u

#define ROGUE_WIN_MAX 128u
#define ROGUE_WINDOW_MS 8000u
#define ROGUE_HOP_TICKS 5u /* tick=100ms -> hop ~500ms */

static DefenderApp* s_app;
static bool s_warned;
static bool s_monitoring;
static uint8_t s_channel;
static uint32_t s_hop_ctr;
// No-PSRAM RAM diet: the 6 KB beacon-window is heap-allocated only while
// monitoring runs (alloc in start_monitoring, freed in on_exit). GUI-thread-only
// access (tick handler drains the tap into it), so plain malloc/free + NULL-guards.
static DefenderBeaconRec* s_win;
static uint32_t s_win_count;

static void defender_rogue_warn_cb(void* context, uint32_t index) {
    DefenderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

static void defender_rogue_evict_stale(uint32_t now_ms) {
    if(!s_win) {
        s_win_count = 0;
        return;
    }
    uint32_t kept = 0;
    for(uint32_t i = 0; i < s_win_count; i++) {
        if((uint32_t)(now_ms - s_win[i].ts_ms) < ROGUE_WINDOW_MS) {
            if(kept != i) s_win[kept] = s_win[i];
            kept++;
        }
    }
    s_win_count = kept;
}

static void defender_rogue_append(const DefenderBeaconRec* rec) {
    if(!s_win) return;
    if(s_win_count >= ROGUE_WIN_MAX) {
        memmove(&s_win[0], &s_win[1], (ROGUE_WIN_MAX - 1) * sizeof(DefenderBeaconRec));
        s_win_count = ROGUE_WIN_MAX - 1;
    }
    s_win[s_win_count++] = *rec;
}

static void defender_rogue_start_monitoring(DefenderApp* app) {
    // Alloc the 6 KB beacon window for this session (freed in on_exit). If the heap
    // can't spare it, monitoring still runs; the window stays empty via NULL-guards.
    if(!s_win) s_win = malloc(sizeof(DefenderBeaconRec) * ROGUE_WIN_MAX);
    s_monitoring = true;
    s_channel = 1;
    s_hop_ctr = 0;
    s_win_count = 0;

    widget_reset(app->widget);
    widget_add_string_multiline_element(
        app->widget, 2, 12, AlignLeft, AlignTop, FontSecondary, "Rogue AP\nScanning...");
    defender_wifi_tap_start(s_channel);
    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewWidget);
}

void defender_scene_rogue_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    s_app = app;

    if(!s_warned) {
        s_monitoring = false;
        submenu_reset(app->submenu);
        submenu_set_header(app->submenu, "WiFi disables BLE");
        submenu_add_item(app->submenu, "Continue", ROGUE_WARN_CONTINUE, defender_rogue_warn_cb, app);
        submenu_add_item(app->submenu, "Back", ROGUE_WARN_BACK, defender_rogue_warn_cb, app);
        view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewSubmenu);
        return;
    }
    defender_rogue_start_monitoring(app);
}

bool defender_scene_rogue_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == ROGUE_WARN_CONTINUE) {
            s_warned = true;
            defender_rogue_start_monitoring(app);
            consumed = true;
        } else if(event.event == ROGUE_WARN_BACK) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick && s_monitoring) {
        uint32_t now = furi_get_tick();

        // Channel hop to cover the band (evil twins may live on different channels).
        if((++s_hop_ctr % ROGUE_HOP_TICKS) == 0) {
            s_channel = (s_channel >= 13) ? 1 : (uint8_t)(s_channel + 1);
            defender_wifi_tap_set_channel(s_channel);
        }

        // Drain beacons into the sliding window.
        DefenderBeaconRec tmp[32];
        uint32_t n = defender_wifi_tap_drain_beacon(tmp, 32);
        defender_rogue_evict_stale(now);
        for(uint32_t i = 0; i < n; i++) defender_rogue_append(&tmp[i]);

        DefenderRogueResult res;
        memset(&res, 0, sizeof(res));
        if(s_win) defender_rogue_eval(s_win, s_win_count, now, ROGUE_WINDOW_MS, &res);

        // Render status.
        char buf[160];
        int w = snprintf(
            buf,
            sizeof(buf),
            "Rogue AP  ch%u\nAPs:%u  SSIDs:%u\n",
            s_channel,
            (unsigned)res.ap_count,
            (unsigned)res.distinct_ssids);
        size_t pos = (w > 0) ? (size_t)w : 0;
        if(res.eviltwin_alert) {
            pos += snprintf(
                buf + pos,
                sizeof(buf) - pos,
                "! EVIL TWIN\n %s x%u\n",
                res.eviltwin_ssid,
                (unsigned)res.eviltwin_bssids);
        }
        if(res.flood_alert) {
            pos += snprintf(
                buf + pos, sizeof(buf) - pos, "! BEACON FLOOD\n %u SSIDs\n", (unsigned)res.distinct_ssids);
        }
        if(!res.eviltwin_alert && !res.flood_alert) {
            snprintf(buf + pos, sizeof(buf) - pos, "clear");
        }
        widget_reset(app->widget);
        widget_add_string_multiline_element(
            app->widget, 2, 10, AlignLeft, AlignTop, FontSecondary, buf);

        // Raise debounced alerts.
        if(res.eviltwin_alert) {
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatWifiRogueAp;
            a.severity = 2;
            a.ts_ms = now;
            snprintf(
                a.msg, sizeof(a.msg), "Evil twin: %.24s x%u", res.eviltwin_ssid, res.eviltwin_bssids);
            defender_alert_raise(&app->alert, &a);
        }
        if(res.flood_alert) {
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatWifiBeaconFlood;
            a.severity = 2;
            a.ts_ms = now;
            snprintf(a.msg, sizeof(a.msg), "Beacon flood: %u SSIDs", res.distinct_ssids);
            defender_alert_raise(&app->alert, &a);
        }
        consumed = true;
    }

    return consumed;
}

void defender_scene_rogue_on_exit(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    if(s_monitoring) {
        defender_wifi_tap_stop();
        s_monitoring = false;
    }
    s_win_count = 0;
    if(s_win) {
        free(s_win);
        s_win = NULL;
    }
    widget_reset(app->widget);
    furi_hal_light_blink_stop();
}
