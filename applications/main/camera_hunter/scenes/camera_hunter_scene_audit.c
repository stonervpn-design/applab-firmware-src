#include "../camera_hunter_app_i.h"
#include "../views/camera_hunter_view_status.h"
#include <furi.h>
#include <string.h>

// On-demand audit of the camera selected in the results list. NO-JOIN scope:
// only a camera on its OWN AP is reachable — cam_audit_start() connects to that
// hotspot (open / default-WPA) and runs the port + default-credential probe,
// appending the results to the findings table. Live phase shows on the status
// view; when the audit finishes we drop back to the results list, which rebuilds
// with the new Audited rows.

void camera_hunter_scene_audit_on_enter(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;

    CamHuntStatus init;
    memset(&init, 0, sizeof(init));
    strncpy(init.phase_text, "Auditing camera...", sizeof(init.phase_text) - 1);
    camera_hunter_view_status_update(app->view_status, &init);
    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewStatus);

    if(!cam_audit_start(app->hunt, app->result_index)) {
        // Not own-AP-auditable (client cam) or an audit is already running —
        // return to the detail/list rather than sit on a dead status screen.
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, CameraHunterSceneResults);
    }
}

bool camera_hunter_scene_audit_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        CamHuntStatus s = cam_hunt_status(app->hunt);
        strncpy(s.phase_text, cam_audit_phase(app->hunt), sizeof(s.phase_text) - 1);
        s.done = cam_audit_done(app->hunt);
        camera_hunter_view_status_update(app->view_status, &s);
        if(cam_audit_done(app->hunt)) {
            cam_audit_join(app->hunt);
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, CameraHunterSceneResults);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        // Abort the running audit (worker + connect/probe loops watch h->cancel).
        cam_hunt_cancel(app->hunt);
        cam_audit_join(app->hunt);
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, CameraHunterSceneResults);
        consumed = true;
    }

    return consumed;
}

void camera_hunter_scene_audit_on_exit(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    cam_audit_join(app->hunt); // ensure the audit thread is joined + freed
}
