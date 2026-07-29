#include "../qrcode_app_i.h"

#include <furi.h>

static void qrcode_scene_text_input_callback(void* context) {
    furi_assert(context);
    QRCodeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventTextDone);
}

void qrcode_scene_text_on_enter(void* context) {
    QRCodeApp* app = context;
    TextInput* text_input = app->text_input;

    app->input_buf[0] = '\0';

    text_input_set_header_text(text_input, "Enter text/URL");
    text_input_set_result_callback(
        text_input,
        qrcode_scene_text_input_callback,
        app,
        app->input_buf,
        sizeof(app->input_buf),
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewTextInput);
}

bool qrcode_scene_text_on_event(void* context, SceneManagerEvent event) {
    QRCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == QRCodeCustomEventTextDone) {
            strncpy(app->payload, app->input_buf, sizeof(app->payload) - 1);
            app->payload[sizeof(app->payload) - 1] = '\0';
            scene_manager_next_scene(app->scene_manager, QRCodeSceneDisplay);
            consumed = true;
        }
    }
    return consumed;
}

void qrcode_scene_text_on_exit(void* context) {
    QRCodeApp* app = context;
    text_input_reset(app->text_input);
}
