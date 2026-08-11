#include "../camera_hunter_app_i.h"
#include "../views/camera_hunter_view_status.h"
#include <furi.h>

void camera_hunter_scene_hunt_on_enter(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;

    // Fresh run: drop any previous hunt (joins its worker) before starting.
    if(app->hunt) {
        cam_hunt_free(app->hunt);
        app->hunt = NULL;
    }
    app->hunt_finished_sent = false;

    CamHuntStatus init;
    memset(&init, 0, sizeof(init));
    strncpy(init.phase_text, "Starting...", sizeof(init.phase_text) - 1);
    camera_hunter_view_status_update(app->view_status, &init);

    app->hunt = cam_hunt_start(app->pending_mode, app->ssid, app->pass);
    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewStatus);
}

bool camera_hunter_scene_hunt_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        if(app->hunt) {
            CamHuntStatus s = cam_hunt_status(app->hunt);
            camera_hunter_view_status_update(app->view_status, &s);
            if(s.done && !app->hunt_finished_sent) {
                app->hunt_finished_sent = true;
                scene_manager_next_scene(app->scene_manager, CameraHunterSceneResults);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        // Leave the running hunt; return straight to the main menu.
        if(app->hunt && !app->hunt_finished_sent) cam_hunt_cancel(app->hunt);
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, CameraHunterSceneMain);
        consumed = true;
    }

    return consumed;
}

void camera_hunter_scene_hunt_on_exit(void* context) {
    furi_assert(context);
    UNUSED(context);
}
