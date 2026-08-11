#include "camera_hunter_app_i.h"
#include "views/camera_hunter_view_status.h"

static bool camera_hunter_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool camera_hunter_back_event_callback(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void camera_hunter_tick_event_callback(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static CameraHunterApp* camera_hunter_app_alloc(void) {
    CameraHunterApp* app = malloc(sizeof(CameraHunterApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    memset(app, 0, sizeof(CameraHunterApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&camera_hunter_scene_handlers, app);

    app->submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->view_status = camera_hunter_view_status_alloc();
    app->widget = widget_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, camera_hunter_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, camera_hunter_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, camera_hunter_tick_event_callback, 250);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(
        app->view_dispatcher, CamHunterViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher, CamHunterViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, CamHunterViewStatus, app->view_status);
    view_dispatcher_add_view(
        app->view_dispatcher, CamHunterViewWidget, widget_get_view(app->widget));

    scene_manager_next_scene(app->scene_manager, CameraHunterSceneMain);
    return app;
}

static void camera_hunter_app_free(CameraHunterApp* app) {
    if(app->hunt) cam_hunt_free(app->hunt);

    scene_manager_free(app->scene_manager);

    view_dispatcher_remove_view(app->view_dispatcher, CamHunterViewWidget);
    widget_free(app->widget);
    view_dispatcher_remove_view(app->view_dispatcher, CamHunterViewStatus);
    camera_hunter_view_status_free(app->view_status);
    view_dispatcher_remove_view(app->view_dispatcher, CamHunterViewTextInput);
    text_input_free(app->text_input);
    view_dispatcher_remove_view(app->view_dispatcher, CamHunterViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t camera_hunter_app(void* p) {
    UNUSED(p);
    CameraHunterApp* app = camera_hunter_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    camera_hunter_app_free(app);
    return 0;
}
