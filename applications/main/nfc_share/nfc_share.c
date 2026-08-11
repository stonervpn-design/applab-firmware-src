#include "nfc_share_i.h"

static bool nfc_share_custom_cb(void* ctx, uint32_t event) {
    NfcShareApp* app = ctx;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}
static bool nfc_share_back_cb(void* ctx) {
    NfcShareApp* app = ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}
static void nfc_share_tick_cb(void* ctx) {
    NfcShareApp* app = ctx;
    scene_manager_handle_tick_event(app->scene_manager);
}

static NfcShareApp* nfc_share_app_alloc(void) {
    NfcShareApp* app = malloc(sizeof(NfcShareApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    memset(app, 0, sizeof(NfcShareApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->file_path = furi_string_alloc();
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&nfc_share_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nfc_share_custom_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nfc_share_back_cb);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, nfc_share_tick_cb, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, NfcShareViewSubmenu, submenu_get_view(app->submenu));
    app->progress = nfc_share_progress_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, NfcShareViewProgress, nfc_share_progress_view_get_view(app->progress));
    scene_manager_next_scene(app->scene_manager, NfcShareSceneStart);
    return app;
}

static void nfc_share_app_free(NfcShareApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, NfcShareViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, NfcShareViewProgress);
    nfc_share_progress_view_free(app->progress);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_string_free(app->file_path);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t nfc_share_app(void* p) {
    UNUSED(p);
    NfcShareApp* app = nfc_share_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    nfc_share_app_free(app);
    return 0;
}
