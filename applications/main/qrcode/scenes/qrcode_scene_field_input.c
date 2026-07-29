#include "../qrcode_app_i.h"

#include <furi.h>

static void qrcode_scene_field_input_callback(void* context) {
    furi_assert(context);
    QRCodeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventFieldDone);
}

/** Resolve the target buffer + size + header for the field being edited. */
static char* qrcode_field_target(QRCodeApp* app, size_t* size, const char** header) {
    switch(app->edit_field) {
    case QRCodeFieldSsid:
        *size = sizeof(app->ssid);
        *header = "Enter SSID";
        return app->ssid;
    case QRCodeFieldPass:
        *size = sizeof(app->pass);
        *header = "Enter Password";
        return app->pass;
    case QRCodeFieldName:
        *size = sizeof(app->name);
        *header = "Enter Name";
        return app->name;
    case QRCodeFieldPhone:
        *size = sizeof(app->phone);
        *header = "Enter Phone";
        return app->phone;
    case QRCodeFieldEmail:
    default:
        *size = sizeof(app->email);
        *header = "Enter Email";
        return app->email;
    }
}

void qrcode_scene_field_input_on_enter(void* context) {
    QRCodeApp* app = context;
    TextInput* text_input = app->text_input;

    size_t size = 0;
    const char* header = "";
    char* target = qrcode_field_target(app, &size, &header);

    text_input_set_header_text(text_input, header);
    text_input_set_result_callback(
        text_input, qrcode_scene_field_input_callback, app, target, size, false);

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewTextInput);
}

bool qrcode_scene_field_input_on_event(void* context, SceneManagerEvent event) {
    QRCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == QRCodeCustomEventFieldDone) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }
    return consumed;
}

void qrcode_scene_field_input_on_exit(void* context) {
    QRCodeApp* app = context;
    text_input_reset(app->text_input);
}
