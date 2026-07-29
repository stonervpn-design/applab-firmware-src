#include "wiegand_app_i.h"
#include <furi.h>

static bool wiegand_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    WiegandApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool wiegand_app_back_event_callback(void* context) {
    furi_assert(context);
    WiegandApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void wiegand_app_tick_event_callback(void* context) {
    furi_assert(context);
    WiegandApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

WiegandApp* wiegand_app_alloc(void) {
    WiegandApp* app = malloc(sizeof(WiegandApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&wiegand_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wiegand_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wiegand_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, wiegand_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WiegandViewSubmenu, submenu_get_view(app->submenu));
    app->advanced_unlocked = false;
    scene_manager_next_scene(app->scene_manager, WiegandSceneStart);
    return app;
}

void wiegand_app_free(WiegandApp* app) {
    furi_assert(app);
    view_dispatcher_remove_view(app->view_dispatcher, WiegandViewSubmenu);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t wiegand_app(void* p) {
    UNUSED(p);
    WiegandApp* app = wiegand_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    wiegand_app_free(app);
    return 0;
}
