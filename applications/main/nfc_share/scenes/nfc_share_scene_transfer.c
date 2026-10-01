#include "../nfc_share_i.h"

void nfc_share_scene_transfer_on_enter(void* context) {
    NfcShareApp* app = context;
    app->engine = nfc_share_engine_alloc(
        app->role,
        app->role == NfcShareRoleSend ? furi_string_get_cstr(app->file_path) :
                                         NFC_SHARE_RECV_FOLDER);
    nfc_share_engine_start(app->engine);
    view_dispatcher_switch_to_view(app->view_dispatcher, NfcShareViewProgress);
}

bool nfc_share_scene_transfer_on_event(void* context, SceneManagerEvent event) {
    NfcShareApp* app = context;
    if(event.type == SceneManagerEventTypeTick) {
        NfcShareStatus s;
        nfc_share_engine_status(app->engine, &s);
        nfc_share_progress_view_update(app->progress, &s, app->role == NfcShareRoleSend);
        return true;
    }
    return false;
}

void nfc_share_scene_transfer_on_exit(void* context) {
    NfcShareApp* app = context;
    nfc_share_engine_stop(app->engine);
    nfc_share_engine_free(app->engine);
    app->engine = NULL;
}
