#include "../wiegand_app_i.h"

enum WiegandStartMenuIndex {
    WiegandStartMenuCapture,
    WiegandStartMenuSavedCaptures,
    WiegandStartMenuCraft,
};

static void wiegand_scene_start_menu_callback(void* context, uint32_t index) {
    WiegandApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wiegand_scene_start_on_enter(void* context) {
    furi_assert(context);
    WiegandApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(app->submenu, "Capture", WiegandStartMenuCapture, wiegand_scene_start_menu_callback, app);
    submenu_add_item(app->submenu, "Saved Captures", WiegandStartMenuSavedCaptures, wiegand_scene_start_menu_callback, app);
    submenu_add_item(app->submenu, "Craft Credential", WiegandStartMenuCraft, wiegand_scene_start_menu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WiegandViewSubmenu);
}

bool wiegand_scene_start_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    WiegandApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
            case WiegandStartMenuCapture:
                scene_manager_next_scene(app->scene_manager, WiegandSceneCapture);
                break;
            case WiegandStartMenuSavedCaptures:
                // TODO: saved captures scene
                consumed = false;
                break;
            case WiegandStartMenuCraft:
                scene_manager_next_scene(app->scene_manager, WiegandSceneCraft);
                break;
            default:
                consumed = false;
                break;
        }
    }
    return consumed;
}

void wiegand_scene_start_on_exit(void* context) {
    furi_assert(context);
    WiegandApp* app = context;
    submenu_reset(app->submenu);
}
