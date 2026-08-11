#include "../camera_hunter_app_i.h"
#include <furi.h>

// Authorized-use gate. Camera Hunter passively sniffs the air for camera-vendor
// devices, scans for camera Wi-Fi APs, and connects only to a camera's OWN
// open/default-key AP to audit it — it never joins the operator's networks. The
// operator must confirm they are testing only equipment they own or may assess.

static void camera_hunter_authorize_button_cb(GuiButtonType result, InputType type, void* context) {
    CameraHunterApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        scene_manager_handle_custom_event(app->scene_manager, CamHunterEvtAgree);
    }
}

void camera_hunter_scene_authorize_on_enter(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;

    widget_reset(app->widget);
    widget_add_text_scroll_element(
        app->widget,
        0,
        0,
        128,
        52,
        "\e#Authorized Use Only\e#\n"
        "This tool passively sniffs the air for camera-vendor devices and scans "
        "for camera Wi-Fi. It NEVER joins your networks — it only connects to a "
        "camera's OWN open/default AP to audit it with DEFAULT logins over "
        "HTTP/RTSP. Use ONLY on cameras you own or have written permission to "
        "test. Unauthorized access is illegal.");
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Agree", camera_hunter_authorize_button_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewWidget);
}

bool camera_hunter_scene_authorize_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == CamHunterEvtAgree) {
        app->authorized = true;
        scene_manager_previous_scene(app->scene_manager); // back to main; user re-picks a mode
        consumed = true;
    }

    return consumed;
}

void camera_hunter_scene_authorize_on_exit(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    widget_reset(app->widget);
}
