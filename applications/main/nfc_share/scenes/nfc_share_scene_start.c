#include "../nfc_share_i.h"
static void submenu_cb(void* ctx, uint32_t index) {
    NfcShareApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}
void nfc_share_scene_start_on_enter(void* context) {
    NfcShareApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(app->submenu, "Send a file", 0, submenu_cb, app);
    submenu_add_item(app->submenu, "Receive a file", 1, submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, NfcShareViewSubmenu);
}
bool nfc_share_scene_start_on_event(void* context, SceneManagerEvent event) {
    NfcShareApp* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == 0) {
            app->role = NfcShareRoleSend;
            scene_manager_next_scene(app->scene_manager, NfcShareSceneFileSelect);
        } else {
            app->role = NfcShareRoleReceive;
            scene_manager_next_scene(app->scene_manager, NfcShareSceneTransfer);
        }
        return true;
    }
    return false;
}
void nfc_share_scene_start_on_exit(void* context) {
    NfcShareApp* app = context;
    submenu_reset(app->submenu);
}
