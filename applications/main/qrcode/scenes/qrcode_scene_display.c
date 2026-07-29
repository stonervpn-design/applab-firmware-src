#include "../qrcode_app_i.h"

void qrcode_scene_display_on_enter(void* context) {
    QRCodeApp* app = context;

    qrcode_display_set_payload(app->qrcode_display, app->payload);

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewDisplay);
}

bool qrcode_scene_display_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    // Back is handled by the view dispatcher navigation callback, which returns
    // to the previous scene. Nothing custom to consume here.
    return false;
}

void qrcode_scene_display_on_exit(void* context) {
    UNUSED(context);
}
