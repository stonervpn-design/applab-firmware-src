#include "../qrcode_app_i.h"

#include <furi.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <string.h>

#define QRCODE_FILE_START_DIR "/ext"
#define QRCODE_FILE_MESSAGE_PREFIX "Message:"

/* Read the chosen file into app->payload (bounded, NUL-terminated).
 * Upstream .qrcode files store the payload as "Message:<text>" on the first
 * line; strip that prefix and keep only that line. Otherwise use the raw
 * bytes. A trailing CR/LF is trimmed in either case. */
static void qrcode_scene_file_load(QRCodeApp* app, const char* path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    app->payload[0] = '\0';

    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t n = storage_file_read(file, app->payload, sizeof(app->payload) - 1);
        app->payload[n] = '\0';

        const size_t prefix_len = strlen(QRCODE_FILE_MESSAGE_PREFIX);
        if(strncmp(app->payload, QRCODE_FILE_MESSAGE_PREFIX, prefix_len) == 0) {
            /* Keep only the first line, then drop the "Message:" prefix. */
            char* nl = strchr(app->payload, '\n');
            if(nl) *nl = '\0';
            memmove(
                app->payload,
                app->payload + prefix_len,
                strlen(app->payload + prefix_len) + 1);
        }

        /* Trim a trailing newline (and CR from CRLF endings). */
        size_t len = strlen(app->payload);
        while(len > 0 && (app->payload[len - 1] == '\n' || app->payload[len - 1] == '\r')) {
            app->payload[--len] = '\0';
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void qrcode_scene_file_on_enter(void* context) {
    QRCodeApp* app = context;

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    FuriString* path = furi_string_alloc_set(QRCODE_FILE_START_DIR);

    DialogsFileBrowserOptions browser_options;
    /* No extension filter so both plain text files and upstream .qrcode
     * files can be selected; default icon. */
    dialog_file_browser_set_basic_options(&browser_options, NULL, NULL);
    browser_options.base_path = QRCODE_FILE_START_DIR;

    bool chosen = dialog_file_browser_show(dialogs, path, path, &browser_options);

    if(chosen) {
        qrcode_scene_file_load(app, furi_string_get_cstr(path));
    }

    furi_string_free(path);
    furi_record_close(RECORD_DIALOGS);

    if(chosen) {
        /* Show the QR for the loaded payload. Back from Display returns here
         * and re-shows the browser (natural re-entry, matches the port's
         * other file-open scenes). */
        scene_manager_next_scene(app->scene_manager, QRCodeSceneDisplay);
    } else {
        /* Cancelled: return to the start submenu. */
        scene_manager_previous_scene(app->scene_manager);
    }
}

bool qrcode_scene_file_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    /* The browser is a blocking modal shown in on_enter; this scene owns no
     * view and has nothing to consume. */
    return false;
}

void qrcode_scene_file_on_exit(void* context) {
    UNUSED(context);
}
