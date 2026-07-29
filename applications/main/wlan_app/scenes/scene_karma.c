#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include <string.h>
#include <stdio.h>

#define KARMA_TICK_MS 250
#define KARMA_TICKS_PER_SEC (1000 / KARMA_TICK_MS)

enum { KarmaEvtToggle = 1 };

static bool s_karma_running;
static uint16_t s_karma_tick;
static uint32_t s_karma_sec;

static void karma_action_cb(WlanSnifferViewAction action, void* ctx) {
    WlanApp* app = ctx;
    if(action == WlanSnifferViewActionToggle) {
        view_dispatcher_send_custom_event(app->view_dispatcher, KarmaEvtToggle);
    }
    // Channel/Targets actions are ignored in Karma.
}

static void karma_record_loot(void) {
    WlanHalKarmaStats st;
    wlan_hal_karma_get_stats(&st);
    if(st.harvested == 0) return;
    char detail[32];
    snprintf(detail, sizeof(detail), "%u harvested", (unsigned)st.harvested);
    wlan_loot_record("karma", st.top_ssid[0] ? st.top_ssid : "(none)", detail);
}

void wlan_app_scene_karma_on_enter(void* context) {
    WlanApp* app = context;
    s_karma_running = false;
    s_karma_tick = 0;
    s_karma_sec = 0;

    wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
    wlan_sniffer_view_set_channel_mode(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_title(app->sniffer_view_obj, "Karma");
    wlan_sniffer_view_set_target_count(app->sniffer_view_obj, 0);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_action_callback(app->sniffer_view_obj, karma_action_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSniffer);
}

bool wlan_app_scene_karma_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == KarmaEvtToggle) {
        if(!s_karma_running) {
            s_karma_tick = 0;
            s_karma_sec = 0;
            wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
            wlan_hal_karma_start();
            s_karma_running = wlan_hal_karma_is_running();
            wlan_sniffer_view_set_running(app->sniffer_view_obj, s_karma_running);
        } else {
            wlan_hal_karma_stop();
            karma_record_loot();
            s_karma_running = false;
            wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick && s_karma_running) {
        s_karma_tick++;
        if(s_karma_tick % KARMA_TICKS_PER_SEC == 0) s_karma_sec++;
        WlanHalKarmaStats st;
        wlan_hal_karma_get_stats(&st);
        wlan_sniffer_view_set_received(app->sniffer_view_obj, st.beacons_sent);
        wlan_sniffer_view_set_elapsed(app->sniffer_view_obj, s_karma_sec);
        wlan_sniffer_view_set_target_count(app->sniffer_view_obj, (uint8_t)st.harvested);
    }
    return consumed;
}

void wlan_app_scene_karma_on_exit(void* context) {
    WlanApp* app = context;
    if(wlan_hal_karma_is_running()) {
        wlan_hal_karma_stop();
        karma_record_loot();
    }
    wlan_sniffer_view_set_action_callback(app->sniffer_view_obj, NULL, NULL);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    s_karma_running = false;
}
