#include "../defender_app_i.h"
#include "../defender_ble_tracker.h"
#include "../../ble_spam/ble_walk_hal.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

/**
 * BLE unwanted-tracker detector scene.
 *
 * Passive BLE scan (BLE_SCAN_TYPE_PASSIVE via ble_tracker_hal). Each tick polls
 * the HAL device table through the pure decision module (defender_ble_tracker_poll)
 * and raises a debounced DefenderCatBleTracker alert on the strongest offender.
 *
 * Radio discipline: the BT stack is acquired via ble_walk_hal_start() on enter
 * and released via ble_walk_hal_stop() on exit, mirroring the ble_spam tracker
 * scan scene. Receive-only.
 */

#define BLE_TRACKER_OK_LOCATE 210u

static DefenderApp* s_app;
static bool s_active;

static bool defender_ble_tracker_ok_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(s_app->view_dispatcher, BLE_TRACKER_OK_LOCATE);
        return true;
    }
    return false;
}

void defender_scene_ble_tracker_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    s_app = app;

    DefenderRatemeterModel* m = view_get_model(app->ratemeter);
    memset(m, 0, sizeof(DefenderRatemeterModel));
    snprintf(m->title, sizeof(m->title), "BLE Tracker");
    view_commit_model(app->ratemeter, true);
    view_set_input_callback(app->ratemeter, defender_ble_tracker_ok_input);
    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewRateMeter);

    s_active = ble_walk_hal_start();
    if(s_active) {
        ble_tracker_hal_set_sweep(false);
        ble_tracker_hal_start_scan();
    }
}

bool defender_scene_ble_tracker_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BLE_TRACKER_OK_LOCATE) {
            DefenderRatemeterModel* m = view_get_model(app->ratemeter);
            snprintf(
                app->locator_label,
                sizeof(app->locator_label),
                "Tag %02X:%02X:%02X",
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
    } else if(event.type == SceneManagerEventTypeTick && s_active) {
        uint16_t count = 0;
        TrackerDevice* devs = ble_tracker_hal_get_devices(&count);
        uint32_t now = furi_get_tick();

        int idx = defender_ble_tracker_poll(devs, count, now);

        DefenderRatemeterModel* m = view_get_model(app->ratemeter);
        m->rate = count;
        m->alert = (idx >= 0);
        if(idx >= 0) {
            memcpy(m->top_bssid, devs[idx].addr, 6);
            m->top_count = 1;
            m->top_rssi = devs[idx].rssi;
        }
        view_commit_model(app->ratemeter, true);

        if(idx >= 0) {
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatBleTracker;
            a.severity = 2;
            memcpy(a.offender, devs[idx].addr, 6);
            a.rssi = devs[idx].rssi;
            a.ts_ms = now;
            snprintf(
                a.msg,
                sizeof(a.msg),
                "Tracker %s follows",
                ble_tracker_kind_label(devs[idx].kind));
            defender_alert_raise(&app->alert, &a);
        }
        consumed = true;
    }

    return consumed;
}

void defender_scene_ble_tracker_on_exit(void* context) {
    furi_assert(context);
    UNUSED(context);
    if(s_active) {
        ble_tracker_hal_stop_scan();
        ble_walk_hal_stop();
        s_active = false;
    }
    furi_hal_light_blink_stop();  // Belt-and-suspenders: clear any lingering blink on scene exit.
}
