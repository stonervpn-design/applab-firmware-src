#include "lan_attack_app_i.h"
#include "views/lan_attack_view_scan_status.h"

static bool lan_attack_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    LanAttackApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool lan_attack_app_back_event_callback(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void lan_attack_app_tick_event_callback(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static LanAttackApp* lan_attack_app_alloc(void) {
    LanAttackApp* app = malloc(sizeof(LanAttackApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&lan_attack_scene_handlers, app);
    app->submenu = submenu_alloc();
    app->view_scan_status = lan_attack_view_scan_status_alloc();
    app->widget = widget_alloc();
    app->recon = NULL;
    app->authorized = false;

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, lan_attack_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, lan_attack_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, lan_attack_app_tick_event_callback, 200);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, LanAttackViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, LanAttackViewScanStatus, app->view_scan_status);
    view_dispatcher_add_view(app->view_dispatcher, LanAttackViewWidget, widget_get_view(app->widget));

    scene_manager_next_scene(app->scene_manager, LanAttackSceneMain);
    return app;
}

static void lan_attack_app_free(LanAttackApp* app) {
    scene_manager_free(app->scene_manager);
    view_dispatcher_remove_view(app->view_dispatcher, LanAttackViewWidget);
    widget_free(app->widget);
    view_dispatcher_remove_view(app->view_dispatcher, LanAttackViewScanStatus);
    lan_attack_view_scan_status_free(app->view_scan_status);
    view_dispatcher_remove_view(app->view_dispatcher, LanAttackViewSubmenu);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t lan_attack_app(void* p) {
    UNUSED(p);
    LanAttackApp* app = lan_attack_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    lan_attack_app_free(app);
    return 0;
}
