#include "../nfc_share_i.h"

static bool nfc_share_file_select(NfcShareApp* app) {
    furi_assert(app);

    DialogsFileBrowserOptions opt;
    dialog_file_browser_set_basic_options(&opt, NULL, NULL);
    opt.base_path = NFC_SHARE_BASE_FOLDER;

    // Input events and views are managed by file_browser
    bool res = dialog_file_browser_show(app->dialogs, app->file_path, app->file_path, &opt);

    return res;
}

void nfc_share_scene_file_select_on_enter(void* context) {
    NfcShareApp* app = context;

    if(nfc_share_file_select(app)) {
        scene_manager_next_scene(app->scene_manager, NfcShareSceneTransfer);
    } else {
        view_dispatcher_stop(app->view_dispatcher);
    }
}

bool nfc_share_scene_file_select_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nfc_share_scene_file_select_on_exit(void* context) {
    UNUSED(context);
}
