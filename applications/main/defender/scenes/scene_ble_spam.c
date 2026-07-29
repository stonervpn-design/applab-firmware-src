#include "../defender_app_i.h"
#include "../defender_ble_spam.h"
#include "../../ble_spam/ble_tracker_hal.h"
#include "../../ble_spam/ble_walk_hal.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

/**
 * BLE-spam flood detector scene.
 *
 * Uses the tracker HAL in *sweep* mode so every advertiser (not just known
 * brands) lands in the device table, then each tick maps the table into
 * DefenderBleSpamRec records and runs the pure defender_blespam_eval. company_id
 * / service_uuid are derived from the classified TrackerKind; is_random_mac from
 * the BLE address type. On this HAL the dominant trigger is the distinct
 * random-MAC flood (the table caps at TRACKER_MAX_DEVICES).
 *
 * Radio discipline: BT stack acquired on enter, released on exit. Receive-only.
 */

#define BLE_SPAM_OK_LOCATE 220u

static DefenderApp* s_app;
static bool s_active;

static bool defender_ble_spam_ok_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(s_app->view_dispatcher, BLE_SPAM_OK_LOCATE);
        return true;
    }
    return false;
}

// Map the classified brand to the manufacturer/service IDs the eval counts.
static void defender_ble_spam_ids(TrackerKind k, uint16_t* company_id, uint16_t* service_uuid) {
    *company_id = 0;
    *service_uuid = 0;
    switch(k) {
    case TrackerKindAirTag:
    case TrackerKindApplePP:
    case TrackerKindAppleNearby:
        *company_id = BLE_CID_APPLE; // Continuity family
        break;
    case TrackerKindSmartTag:
        *company_id = BLE_CID_SAMSUNG;
        break;
    default:
        break;
    }
}

void defender_scene_ble_spam_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    s_app = app;

    DefenderRatemeterModel* m = view_get_model(app->ratemeter);
    memset(m, 0, sizeof(DefenderRatemeterModel));
    snprintf(m->title, sizeof(m->title), "BLE Spam");
    view_commit_model(app->ratemeter, true);
    view_set_input_callback(app->ratemeter, defender_ble_spam_ok_input);
    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewRateMeter);

    s_active = ble_walk_hal_start();
    if(s_active) {
        ble_tracker_hal_set_sweep(true); // capture ALL advertisers
        ble_tracker_hal_start_scan();
    }
}

bool defender_scene_ble_spam_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == BLE_SPAM_OK_LOCATE) {
            snprintf(app->locator_label, sizeof(app->locator_label), "BLE Spam src");
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
        if(count > TRACKER_MAX_DEVICES) count = TRACKER_MAX_DEVICES;

        DefenderBleSpamRec recs[TRACKER_MAX_DEVICES];
        for(uint16_t i = 0; i < count; i++) {
            memset(&recs[i], 0, sizeof(recs[i]));
            memcpy(recs[i].addr, devs[i].addr, 6);
            recs[i].is_random_mac =
                (devs[i].addr_type == BLE_ADDR_TYPE_RANDOM ||
                 devs[i].addr_type == BLE_ADDR_TYPE_RPA_RANDOM);
            defender_ble_spam_ids(
                devs[i].kind, &recs[i].company_id, &recs[i].service_uuid);
            recs[i].ts_ms = devs[i].last_seen_ms;
        }

        DefenderBleSpamResult res;
        defender_blespam_eval(
            recs, count, now, BLE_SPAM_WINDOW_MS, BLE_SPAM_ALERT, BLE_MAC_FLOOD_ALERT, &res);

        uint16_t spam_total = res.continuity + res.fastpair + res.swiftpair;

        DefenderRatemeterModel* m = view_get_model(app->ratemeter);
        m->rate = spam_total;
        m->top_count = res.distinct_random_macs;
        m->top_rssi = 0;
        memset(m->top_bssid, 0, 6);
        m->alert = res.alert;
        view_commit_model(app->ratemeter, true);

        if(res.alert) {
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatBleSpam;
            a.severity = 2;
            a.ts_ms = now;
            snprintf(
                a.msg,
                sizeof(a.msg),
                "BLE spam %u adv/%u MAC",
                (unsigned)spam_total,
                (unsigned)res.distinct_random_macs);
            defender_alert_raise(&app->alert, &a);
        }
        consumed = true;
    }

    return consumed;
}

void defender_scene_ble_spam_on_exit(void* context) {
    furi_assert(context);
    UNUSED(context);
    if(s_active) {
        ble_tracker_hal_stop_scan();
        ble_tracker_hal_set_sweep(false);
        ble_walk_hal_stop();
        s_active = false;
    }
    furi_hal_light_blink_stop();  // Belt-and-suspenders: clear any lingering blink on scene exit.
}
