#include "../ble_spam_app.h"
#include "../tracker_category.h"

static void tracker_pick_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_tracker_pick_on_enter(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
    for(uint32_t i = 0; i < TrackerCatCount; i++) {
        submenu_add_item(
            app->submenu, tracker_category_label((TrackerCategory)i), i,
            tracker_pick_callback, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_tracker_pick_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event < TrackerCatCount) {
            app->tracker_category = (TrackerCategory)event.event;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerScan);
            consumed = true;
        }
    }
    return consumed;
}

void ble_spam_scene_tracker_pick_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
