#include "can_commander_app_i.h"
#include <string.h>

bool can_commander_parse_input(const char* s, CanFrame* out) {
    if(!s || !out) return false;
    // Split "ID#DATA" on '#'. No '#' -> whole string is the id, no data.
    char idbuf[16];
    const char* hash = strchr(s, '#');
    size_t idlen = hash ? (size_t)(hash - s) : strlen(s);
    if(idlen == 0 || idlen >= sizeof(idbuf)) return false;
    memcpy(idbuf, s, idlen);
    idbuf[idlen] = '\0';
    const char* data = hash ? hash + 1 : "";
    // RTR shorthand: "ID#R" or "ID#r"
    bool rtr = (data[0] == 'R' || data[0] == 'r') && data[1] == '\0';
    return can_frame_build(idbuf, rtr ? "" : data, false, rtr, out);
}

static bool can_commander_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool can_commander_back_event_callback(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void can_commander_tick_event_callback(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static CanCommanderApp* can_commander_app_alloc(void) {
    CanCommanderApp* app = malloc(sizeof(CanCommanderApp));
    memset(app, 0, sizeof(CanCommanderApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&can_commander_scene_handlers, app);
    app->submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->popup = popup_alloc();
    app->var_item_list = variable_item_list_alloc();
    app->widget = widget_alloc();
    app->mon_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    // Defaults
    app->bitrate = 500000;
    app->mode = CanModeNormal;
    app->tx_gpio = 2; // Grove SDA / G2
    app->rx_gpio = 1; // Grove SCL / G1
    strncpy(app->frame_input, "7DF#02010C", sizeof(app->frame_input) - 1);

    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, can_commander_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, can_commander_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, can_commander_tick_event_callback, 200);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(app->view_dispatcher, CanViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher, CanViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, CanViewPopup, popup_get_view(app->popup));
    view_dispatcher_add_view(
        app->view_dispatcher, CanViewVarItemList, variable_item_list_get_view(app->var_item_list));
    view_dispatcher_add_view(app->view_dispatcher, CanViewWidget, widget_get_view(app->widget));

    scene_manager_next_scene(app->scene_manager, CanCommanderSceneMain);
    return app;
}

static void can_commander_app_free(CanCommanderApp* app) {
    // Safety: ensure the TWAI driver is released even if a scene didn't.
    can_hal_stop();

    scene_manager_free(app->scene_manager);

    view_dispatcher_remove_view(app->view_dispatcher, CanViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, CanViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, CanViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, CanViewVarItemList);
    view_dispatcher_remove_view(app->view_dispatcher, CanViewWidget);

    submenu_free(app->submenu);
    text_input_free(app->text_input);
    popup_free(app->popup);
    variable_item_list_free(app->var_item_list);
    widget_free(app->widget);
    view_dispatcher_free(app->view_dispatcher);
    furi_mutex_free(app->mon_mutex);

    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t can_commander_app(void* p) {
    UNUSED(p);
    CanCommanderApp* app = can_commander_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    can_commander_app_free(app);
    return 0;
}
