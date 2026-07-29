#include "../wlan_app.h"
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>

#define LOOT_PREVIEW_CAP 2048

void wlan_app_scene_loot_preview_on_enter(void* context) {
    WlanApp* app = context;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    char* buf = malloc(LOOT_PREVIEW_CAP + 1);

    furi_string_reset(app->text_buf);
    if(buf && storage_file_open(f, app->loot_file, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n = storage_file_read(f, buf, LOOT_PREVIEW_CAP);
        buf[n] = '\0';
        furi_string_set(app->text_buf, buf);
    } else {
        furi_string_set(app->text_buf, "(cannot open file)");
    }
    storage_file_close(f);
    storage_file_free(f);
    if(buf) free(buf);
    furi_record_close(RECORD_STORAGE);

    widget_reset(app->widget);
    widget_add_text_scroll_element(
        app->widget, 0, 0, 128, 64, furi_string_get_cstr(app->text_buf));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_loot_preview_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wlan_app_scene_loot_preview_on_exit(void* context) {
    WlanApp* app = context;
    widget_reset(app->widget);
}
