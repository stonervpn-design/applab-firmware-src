#include "../qrcode_app_i.h"

enum QRCodeStartSubmenuIndex {
    QRCodeStartSubmenuText,
    QRCodeStartSubmenuWifi,
    QRCodeStartSubmenuVcard,
    QRCodeStartSubmenuFile,
};

static void qrcode_scene_start_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    QRCodeApp* app = context;
    if(index == QRCodeStartSubmenuText) {
        view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventStartText);
    } else if(index == QRCodeStartSubmenuWifi) {
        view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventStartWifi);
    } else if(index == QRCodeStartSubmenuVcard) {
        view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventStartVcard);
    } else if(index == QRCodeStartSubmenuFile) {
        view_dispatcher_send_custom_event(app->view_dispatcher, QRCodeCustomEventStartFile);
    }
}

void qrcode_scene_start_on_enter(void* context) {
    QRCodeApp* app = context;
    Submenu* submenu = app->submenu;

    submenu_set_header(submenu, "QR Code");
    submenu_add_item(
        submenu,
        "Text / URL",
        QRCodeStartSubmenuText,
        qrcode_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "WiFi",
        QRCodeStartSubmenuWifi,
        qrcode_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Contact (vCard)",
        QRCodeStartSubmenuVcard,
        qrcode_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "From File",
        QRCodeStartSubmenuFile,
        qrcode_scene_start_submenu_callback,
        app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, QRCodeSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewSubmenu);
}

bool qrcode_scene_start_on_event(void* context, SceneManagerEvent event) {
    QRCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == QRCodeCustomEventStartText) {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneStart, QRCodeStartSubmenuText);
            scene_manager_next_scene(app->scene_manager, QRCodeSceneText);
            consumed = true;
        } else if(event.event == QRCodeCustomEventStartWifi) {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneStart, QRCodeStartSubmenuWifi);
            scene_manager_next_scene(app->scene_manager, QRCodeSceneWifi);
            consumed = true;
        } else if(event.event == QRCodeCustomEventStartVcard) {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneStart, QRCodeStartSubmenuVcard);
            scene_manager_next_scene(app->scene_manager, QRCodeSceneVcard);
            consumed = true;
        } else if(event.event == QRCodeCustomEventStartFile) {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneStart, QRCodeStartSubmenuFile);
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFile);
            consumed = true;
        }
    }
    return consumed;
}

void qrcode_scene_start_on_exit(void* context) {
    QRCodeApp* app = context;
    submenu_reset(app->submenu);
}
