#include "bus_tools_app_i.h"
#include <furi.h>

static bool bus_tools_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    BusToolsApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool bus_tools_app_back_event_callback(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void bus_tools_app_tick_event_callback(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

BusToolsApp* bus_tools_app_alloc(void) {
    BusToolsApp* app = malloc(sizeof(BusToolsApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&bus_tools_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, bus_tools_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, bus_tools_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, bus_tools_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BusToolsViewSubmenu, submenu_get_view(app->submenu));
    app->advanced_unlocked = false;
    app->uart_baud = 115200;
    app->uart_view = bus_tools_uart_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BusToolsViewUartMonitor, app->uart_view);
    scene_manager_next_scene(app->scene_manager, BusToolsSceneStart);
    return app;
}

void bus_tools_app_free(BusToolsApp* app) {
    furi_assert(app);
    view_dispatcher_remove_view(app->view_dispatcher, BusToolsViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, BusToolsViewUartMonitor);
    bus_tools_uart_view_free(app->uart_view);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t bus_tools_app(void* p) {
    UNUSED(p);
    BusToolsApp* app = bus_tools_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    bus_tools_app_free(app);
    return 0;
}
