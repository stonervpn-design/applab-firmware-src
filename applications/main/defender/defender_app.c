#include "defender_app_i.h"

#include <furi.h>
#include <furi_hal_light.h>

static bool defender_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    DefenderApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool defender_app_back_event_callback(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void defender_app_tick_event_callback(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    defender_alert_tick(&app->alert);
    scene_manager_handle_tick_event(app->scene_manager);
}

DefenderApp* defender_app_alloc(void) {
    DefenderApp* app = malloc(sizeof(DefenderApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Shared components: alert bus + SD logger (logger feeds the alert bus).
    defender_alert_init(&app->alert, app->notifications);
    app->logger = defender_logger_open();
    app->alert.logger = app->logger;
    app->wifi_tap_active = false;

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&defender_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, defender_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, defender_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, defender_app_tick_event_callback, 100);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, DefenderViewSubmenu, submenu_get_view(app->submenu));

    app->locator = defender_locator_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, DefenderViewLocator, defender_locator_get_view(app->locator));

    app->ratemeter = defender_ratemeter_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, DefenderViewRateMeter, app->ratemeter);

    app->waterfall = defender_waterfall_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, DefenderViewWaterfall, app->waterfall);

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, DefenderViewWidget, widget_get_view(app->widget));

    app->locator_active = false;

    scene_manager_next_scene(app->scene_manager, DefenderSceneStart);

    return app;
}

void defender_app_free(DefenderApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, DefenderViewWidget);
    widget_free(app->widget);

    view_dispatcher_remove_view(app->view_dispatcher, DefenderViewWaterfall);
    defender_waterfall_view_free(app->waterfall);

    view_dispatcher_remove_view(app->view_dispatcher, DefenderViewRateMeter);
    defender_ratemeter_view_free(app->ratemeter);

    view_dispatcher_remove_view(app->view_dispatcher, DefenderViewLocator);
    defender_locator_free(app->locator);

    view_dispatcher_remove_view(app->view_dispatcher, DefenderViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Stop any lingering alert LED before releasing notifications.
    furi_hal_light_blink_stop();
    if(app->logger) defender_logger_close(app->logger);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    free(app);
}

int32_t defender_app(void* p) {
    UNUSED(p);
    DefenderApp* app = defender_app_alloc();

    view_dispatcher_run(app->view_dispatcher);

    defender_app_free(app);

    return 0;
}
