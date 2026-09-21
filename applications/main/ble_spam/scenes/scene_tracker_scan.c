#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../ble_tracker_hal.h"
#include "../tracker_follow.h"
#include "../views/tracker_list_view.h"
#include "../tracker_category.h"
#include "../tracker_log.h"

#include <esp_log.h>
#include <string.h>
#include <furi_hal_light.h>
#include <furi_hal_resources.h>   /* LightRed enum */
#include <furi_hal_speaker.h>
#include <input/input.h>

#define TAG "BleTrackerScan"

static TrackerLog* s_log;                 // alerts-only SD CSV (NULL if no SD)
static uint8_t s_alerted[TRACKER_MAX_DEVICES][6];  // MACs already alerted (FOLLOW!)
static uint8_t s_alerted_count;
static uint8_t s_beep_ticks;              // countdown to stop the alert beep

void ble_spam_scene_tracker_scan_on_enter(void* context) {
    BleSpamApp* app = context;

    TrackerListModel* model = view_get_model(app->view_tracker_scan);
    memset(model, 0, sizeof(TrackerListModel));
    view_commit_model(app->view_tracker_scan, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTrackerScan);

    if(!ble_walk_hal_start()) {
        ESP_LOGE(TAG, "BT stack acquire failed");
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    s_alerted_count = 0;
    s_beep_ticks = 0;
    ble_tracker_hal_set_sweep(tracker_category_uses_sweep(app->tracker_category));
    s_log = tracker_log_open();

    ble_tracker_hal_start_scan();
}

static bool alerted_contains(const uint8_t* mac) {
    for(int i = 0; i < s_alerted_count; i++)
        if(memcmp(s_alerted[i], mac, 6) == 0) return true;
    return false;
}

static void alerted_add(const uint8_t* mac) {
    if(s_alerted_count < TRACKER_MAX_DEVICES) memcpy(s_alerted[s_alerted_count++], mac, 6);
}

// Rebuild the alerted set to only MACs still FOLLOWING this tick, so a device
// that leaves (goes stale) and later returns can re-alert.
static void alerted_prune(TrackerDevice* devs, uint16_t count, uint32_t now) {
    uint8_t kept = 0;
    for(int i = 0; i < s_alerted_count; i++) {
        for(int j = 0; j < count; j++) {
            if(memcmp(s_alerted[i], devs[j].addr, 6) == 0) {
                if(tracker_follow_state(devs[j].first_seen_ms, devs[j].last_seen_ms, now) ==
                   FollowFollowing) {
                    if(kept != i) memcpy(s_alerted[kept], s_alerted[i], 6);
                    kept++;
                }
                break;
            }
        }
    }
    s_alerted_count = kept;
}

static void fire_alert(void) {
    furi_hal_light_blink_start(LightRed, 0xFF, 200, 400); // non-blocking hardware blink
    if(!furi_hal_speaker_is_mine() && furi_hal_speaker_acquire(50)) {
        furi_hal_speaker_start(880.0f, 0.5f);
        s_beep_ticks = 2;                                 // stopped in the tick handler
    }
}

bool ble_spam_scene_tracker_scan_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        TrackerListModel* model = view_get_model(app->view_tracker_scan);

        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_tracker_scan, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + TRACKER_LIST_ITEMS_ON_SCREEN)
                    model->window_offset =
                        model->selected - TRACKER_LIST_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_tracker_scan, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count > 0 && model->selected < model->count) {
                memcpy(
                    &app->tracker_target,
                    &model->devices[model->selected],
                    sizeof(TrackerDevice));
                view_commit_model(app->view_tracker_scan, false);
                scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerGeiger);
            } else {
                view_commit_model(app->view_tracker_scan, false);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        ble_tracker_hal_stop_scan();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        TrackerDevice* devices = ble_tracker_hal_get_devices(&count);
        uint32_t now = furi_get_tick();

        // Copy into a local array we can sort without disturbing the HAL table.
        TrackerListModel* model = view_get_model(app->view_tracker_scan);

        // Capture the selected device's MAC before the re-sort reshuffles the list,
        // so the highlight can be re-anchored to the same device afterwards.
        uint8_t sel_mac[6];
        bool had_sel = (model->count > 0 && model->selected < model->count);
        if(had_sel) memcpy(sel_mac, model->devices[model->selected].addr, 6);

        // Filter the HAL list down to the chosen category (all variants of it).
        uint16_t fcount = 0;
        for(uint16_t i = 0; i < count && fcount < TRACKER_MAX_DEVICES; i++) {
            if(tracker_category_matches(app->tracker_category, devices[i].kind)) {
                model->devices[fcount++] = devices[i];
            }
        }
        count = fcount;              // the rest of the tick operates on the filtered set
        model->count = count;

        // Insertion sort: FOLLOW! first, then higher best_rssi.
        for(int i = 1; i < count; i++) {
            TrackerDevice key = model->devices[i];
            bool key_follow = tracker_follow_state(key.first_seen_ms, key.last_seen_ms, now) ==
                              FollowFollowing;
            int j = i - 1;
            while(j >= 0) {
                bool jf = tracker_follow_state(
                              model->devices[j].first_seen_ms, model->devices[j].last_seen_ms,
                              now) == FollowFollowing;
                bool swap = (key_follow && !jf) ||
                            (key_follow == jf && key.best_rssi > model->devices[j].best_rssi);
                if(!swap) break;
                model->devices[j + 1] = model->devices[j];
                j--;
            }
            model->devices[j + 1] = key;
        }

        model->now_ms = now;
        model->sweep_mode = tracker_category_uses_sweep(app->tracker_category);

        // Re-anchor the selection to the same device (by MAC) after the re-sort,
        // then fix the scroll window so the highlight stays on-screen.
        if(had_sel) {
            for(int i = 0; i < count; i++) {
                if(memcmp(model->devices[i].addr, sel_mac, 6) == 0) {
                    model->selected = i;
                    break;
                }
            }
        }
        if(count == 0) {
            model->selected = 0;
            model->window_offset = 0;
        } else {
            if(model->selected >= count) model->selected = count - 1;
            if(model->selected < model->window_offset)
                model->window_offset = model->selected;
            else if(model->selected >= model->window_offset + TRACKER_LIST_ITEMS_ON_SCREEN)
                model->window_offset = model->selected - TRACKER_LIST_ITEMS_ON_SCREEN + 1;
        }
        view_commit_model(app->view_tracker_scan, true);

        // Debounced FOLLOW! alert.
        alerted_prune(model->devices, count, now);
        for(int i = 0; i < count; i++) {
            if(tracker_follow_state(model->devices[i].first_seen_ms,
                                    model->devices[i].last_seen_ms, now) == FollowFollowing &&
               !alerted_contains(model->devices[i].addr)) {
                alerted_add(model->devices[i].addr);
                fire_alert();
                tracker_log_alert(s_log, app->tracker_category, &model->devices[i]);
            }
        }

        // Stop the alert beep after its countdown.
        if(s_beep_ticks > 0 && --s_beep_ticks == 0) {
            if(furi_hal_speaker_is_mine()) {
                furi_hal_speaker_stop();
                furi_hal_speaker_release();
            }
        }
    }

    return consumed;
}

void ble_spam_scene_tracker_scan_on_exit(void* context) {
    UNUSED(context);
    tracker_log_close(s_log);
    s_log = NULL;
    furi_hal_light_blink_stop();
    furi_hal_light_set_rgb_all(0, 0, 0);
    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    ble_tracker_hal_stop_scan();
    ble_tracker_hal_release();
}
