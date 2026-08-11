#include "../camera_hunter_app_i.h"
#include <furi.h>

static void camera_hunter_scene_main_cb(void* context, uint32_t index) {
    CameraHunterApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

void camera_hunter_scene_main_on_enter(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Camera Hunter");

    submenu_add_item(
        app->submenu, "Scan for cameras", CamHunterEvtAuto, camera_hunter_scene_main_cb, app);
    if(app->hunt) {
        submenu_add_item(
            app->submenu, "Last Results", CamHunterEvtResults, camera_hunter_scene_main_cb, app);
    }
    submenu_add_item(
        app->submenu, "Authorized Use", CamHunterEvtAuthorize, camera_hunter_scene_main_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewSubmenu);
}

bool camera_hunter_scene_main_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case CamHunterEvtAuto:
            app->pending_mode = CamHuntModeAuto;
            if(app->authorized) {
                scene_manager_next_scene(app->scene_manager, CameraHunterSceneHunt);
            } else {
                app->pending_valid = true;
                scene_manager_next_scene(app->scene_manager, CameraHunterSceneAuthorize);
            }
            consumed = true;
            break;
        case CamHunterEvtResults:
            scene_manager_next_scene(app->scene_manager, CameraHunterSceneResults);
            consumed = true;
            break;
        case CamHunterEvtAuthorize:
            app->pending_valid = false;
            scene_manager_next_scene(app->scene_manager, CameraHunterSceneAuthorize);
            consumed = true;
            break;
        default:
            consumed = true;
            break;
        }
    }

    return consumed;
}

void camera_hunter_scene_main_on_exit(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    submenu_reset(app->submenu);
}
