#include "qrcode_app_i.h"

#include <furi.h>

static bool qrcode_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    QRCodeApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool qrcode_app_back_event_callback(void* context) {
    furi_assert(context);
    QRCodeApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static QRCodeApp* qrcode_app_alloc(void) {
    QRCodeApp* app = malloc(sizeof(QRCodeApp));

    app->payload[0] = '\0';
    app->input_buf[0] = '\0';
    app->ssid[0] = '\0';
    app->pass[0] = '\0';
    app->name[0] = '\0';
    app->phone[0] = '\0';
    app->email[0] = '\0';
    app->wifi_auth = QRCodeWifiAuthWpa;
    app->edit_field = QRCodeFieldSsid;

    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&qrcode_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, qrcode_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, qrcode_app_back_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, QRCodeViewSubmenu, submenu_get_view(app->submenu));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, QRCodeViewTextInput, text_input_get_view(app->text_input));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        QRCodeViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    app->qrcode_display = qrcode_display_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, QRCodeViewDisplay, qrcode_display_get_view(app->qrcode_display));

    scene_manager_next_scene(app->scene_manager, QRCodeSceneStart);

    return app;
}

static void qrcode_app_free(QRCodeApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, QRCodeViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, QRCodeViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, QRCodeViewVarItemList);
    view_dispatcher_remove_view(app->view_dispatcher, QRCodeViewDisplay);

    submenu_free(app->submenu);
    text_input_free(app->text_input);
    variable_item_list_free(app->var_item_list);
    qrcode_display_free(app->qrcode_display);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    furi_record_close(RECORD_GUI);

    free(app);
}

int32_t qrcode_app(void* p) {
    UNUSED(p);
    QRCodeApp* app = qrcode_app_alloc();

    view_dispatcher_run(app->view_dispatcher);

    qrcode_app_free(app);

    return 0;
}
