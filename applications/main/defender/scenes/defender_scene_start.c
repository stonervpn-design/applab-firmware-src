#include "../defender_app_i.h"
#include <furi.h>

static void defender_scene_start_submenu_callback(void* context, uint32_t index) {
    DefenderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

void defender_scene_start_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Defender");

    submenu_add_item(app->submenu, "WiFi Deauth", DefenderEvtDeauth, defender_scene_start_submenu_callback, app);
    submenu_add_item(app->submenu, "Rogue AP", DefenderEvtRogueAp, defender_scene_start_submenu_callback, app);
    submenu_add_item(app->submenu, "BLE Tracker", DefenderEvtBleTracker, defender_scene_start_submenu_callback, app);
    submenu_add_item(app->submenu, "BLE Spam", DefenderEvtBleSpam, defender_scene_start_submenu_callback, app);
    submenu_add_item(app->submenu, "SubGHz Jammer", DefenderEvtJammer, defender_scene_start_submenu_callback, app);
    submenu_add_item(app->submenu, "Locator Test", DefenderEvtLocatorTest, defender_scene_start_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewSubmenu);
}

bool defender_scene_start_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        switch (event.event) {
        case DefenderEvtDeauth:
            scene_manager_next_scene(app->scene_manager, DefenderSceneDeauth);
            consumed = true;
            break;
        case DefenderEvtRogueAp:
            scene_manager_next_scene(app->scene_manager, DefenderSceneRogue);
            consumed = true;
            break;
        case DefenderEvtBleTracker:
            scene_manager_next_scene(app->scene_manager, DefenderSceneBleTracker);
            consumed = true;
            break;
        case DefenderEvtBleSpam:
            scene_manager_next_scene(app->scene_manager, DefenderSceneBleSpam);
            consumed = true;
            break;
        case DefenderEvtJammer:
            scene_manager_next_scene(app->scene_manager, DefenderSceneJammer);
            consumed = true;
            break;
        case DefenderEvtLocatorTest:
            scene_manager_next_scene(app->scene_manager, DefenderSceneLocator);
            consumed = true;
            break;
        default:
            consumed = true;
            break;
        }
    }

    return consumed;
}

void defender_scene_start_on_exit(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    submenu_reset(app->submenu);
}
