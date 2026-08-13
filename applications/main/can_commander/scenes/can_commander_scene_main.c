#include "../can_commander_app_i.h"

static void can_commander_scene_main_submenu_cb(void* context, uint32_t index) {
    CanCommanderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

void can_commander_scene_main_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "CAN Commander");
    submenu_add_item(
        app->submenu, "Send Frame", CanCommanderEvtSend, can_commander_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu,
        "Periodic Send",
        CanCommanderEvtPeriodic,
        can_commander_scene_main_submenu_cb,
        app);
    submenu_add_item(
        app->submenu,
        "Monitor Bus",
        CanCommanderEvtMonitor,
        can_commander_scene_main_submenu_cb,
        app);
    submenu_add_item(
        app->submenu, "Presets", CanCommanderEvtPresets, can_commander_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu,
        "Settings",
        CanCommanderEvtSettings,
        can_commander_scene_main_submenu_cb,
        app);
    submenu_add_item(
        app->submenu, "Fuzzer", CanCommanderEvtFuzz, can_commander_scene_main_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewSubmenu);
}

bool can_commander_scene_main_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case CanCommanderEvtSend:
            scene_manager_next_scene(app->scene_manager, CanCommanderSceneSend);
            consumed = true;
            break;
        case CanCommanderEvtPeriodic:
            scene_manager_next_scene(app->scene_manager, CanCommanderScenePeriodic);
            consumed = true;
            break;
        case CanCommanderEvtMonitor:
            scene_manager_next_scene(app->scene_manager, CanCommanderSceneMonitor);
            consumed = true;
            break;
        case CanCommanderEvtPresets:
            scene_manager_next_scene(app->scene_manager, CanCommanderScenePresets);
            consumed = true;
            break;
        case CanCommanderEvtSettings:
            scene_manager_next_scene(app->scene_manager, CanCommanderSceneSettings);
            consumed = true;
            break;
        case CanCommanderEvtFuzz:
            scene_manager_next_scene(app->scene_manager, CanCommanderSceneFuzz);
            consumed = true;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void can_commander_scene_main_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    submenu_reset(app->submenu);
}
