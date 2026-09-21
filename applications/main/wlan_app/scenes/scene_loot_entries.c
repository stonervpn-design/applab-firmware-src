#include "../wlan_app.h"
#include "../wlan_loot_manifest.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOOT_ROOT "/ext/loot"
#define LOOT_MAX_ENTRIES 48
#define LOOT_MANIFEST_CAP 4096
#define LOOT_MANIFEST_NAME "manifest.csv"

// For each submenu item index, the file it previews when selected. Manifest
// rows all point at the session's manifest.csv; raw files point at themselves.
// Allocated on scene enter, freed on exit (was a ~4.6 KB always-resident BSS
// array) — GUI-thread-only, no worker touches it.
static char (*s_entry_path)[96];
static uint8_t s_entry_count;

// Sentinels — all > LOOT_MAX_ENTRIES so they never collide with an entry index.
enum {
    LootEntryIdxDelete = 900,
    LootEntryIdxEmpty = 901,
    LootEntryIdxDelYes = 902,
    LootEntryIdxDelNo = 903,
};

static void loot_entries_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void loot_session_path(const WlanApp* app, char* out, size_t out_sz) {
    snprintf(out, out_sz, LOOT_ROOT "/%s", app->loot_session);
}

// Add one selectable item mapping its menu index to `preview_path`.
static void loot_add_entry(WlanApp* app, const char* label, const char* preview_path) {
    if(!s_entry_path || s_entry_count >= LOOT_MAX_ENTRIES) return;
    strncpy(s_entry_path[s_entry_count], preview_path, sizeof(s_entry_path[0]) - 1);
    s_entry_path[s_entry_count][sizeof(s_entry_path[0]) - 1] = '\0';
    submenu_add_item(app->submenu, label, s_entry_count, loot_entries_menu_cb, app);
    s_entry_count++;
}

// Parse manifest.csv into "HH:MM type target" rows. Returns rows added. All rows
// preview the manifest file itself.
static uint8_t loot_add_manifest_rows(WlanApp* app, const char* manifest_path) {
    uint8_t added = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    char* buf = malloc(LOOT_MANIFEST_CAP + 1);
    if(buf && storage_file_open(f, manifest_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n = storage_file_read(f, buf, LOOT_MANIFEST_CAP);
        buf[n] = '\0';
        char* save = NULL;
        char* line = strtok_r(buf, "\n", &save);
        while(line && s_entry_count < LOOT_MAX_ENTRIES) {
            WlanManifestRow row;
            if(wlan_manifest_parse_line(line, &row)) {
                // timestamp is "YYYY-MM-DD HH:MM:SS" → show the HH:MM part.
                const char* hhmm = "";
                if(strlen(row.timestamp) >= 16) hhmm = row.timestamp + 11;
                char clock[6] = {0};
                strncpy(clock, hhmm, 5);
                char label[80];
                snprintf(label, sizeof(label), "%s %s %s", clock, row.type, row.target);
                loot_add_entry(app, label, manifest_path);
                added++;
            }
            line = strtok_r(NULL, "\n", &save);
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    if(buf) free(buf);
    furi_record_close(RECORD_STORAGE);
    return added;
}

// List every regular file in the session dir except manifest.csv (when its rows
// were already listed). Each is selectable → preview that file.
static void loot_add_raw_files(WlanApp* app, const char* session_dir, bool skip_manifest) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, session_dir)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              s_entry_count < LOOT_MAX_ENTRIES) {
            if(file_info_is_dir(&info)) continue;
            if(skip_manifest && strcmp(name, LOOT_MANIFEST_NAME) == 0) continue;
            // Build from the bounded parts (session id is a 15-char timestamp,
            // loot filenames are short) so the path provably fits path[96].
            char path[96];
            snprintf(path, sizeof(path), LOOT_ROOT "/%.15s/%.60s", app->loot_session, name);
            loot_add_entry(app, name, path);
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

static void loot_entries_populate(WlanApp* app) {
    widget_reset(app->widget);
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, app->loot_session);
    s_entry_count = 0;

    char session_dir[80];
    loot_session_path(app, session_dir, sizeof(session_dir));
    char manifest_path[96];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", session_dir, LOOT_MANIFEST_NAME);

    uint8_t manifest_rows = loot_add_manifest_rows(app, manifest_path);
    loot_add_raw_files(app, session_dir, manifest_rows > 0);

    submenu_add_separator(app->submenu);
    if(s_entry_count == 0) {
        submenu_add_item(
            app->submenu, "(empty session)", LootEntryIdxEmpty, loot_entries_menu_cb, app);
    }
    submenu_add_item(app->submenu, "Delete session", LootEntryIdxDelete, loot_entries_menu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

static void loot_del_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, LootEntryIdxDelYes);
    }
}

static void loot_del_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, LootEntryIdxDelNo);
    }
}

static void loot_show_delete_confirm(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 12, AlignCenter, AlignBottom, FontPrimary, "Delete session?");
    widget_add_text_box_element(
        app->widget, 0, 16, 128, 22, AlignCenter, AlignTop, app->loot_session, false);
    widget_add_button_element(app->widget, GuiButtonTypeLeft, "No", loot_del_no_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "Yes", loot_del_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

void wlan_app_scene_loot_entries_on_enter(void* context) {
    WlanApp* app = context;
    s_entry_path = malloc(sizeof(*s_entry_path) * LOOT_MAX_ENTRIES);
    if(!s_entry_path) {
        // Out of RAM — show an empty list rather than crash; user can back out.
        s_entry_count = 0;
        submenu_reset(app->submenu);
        submenu_add_item(app->submenu, "Low memory", LootEntryIdxEmpty,
            loot_entries_menu_cb, app);
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
        return;
    }
    loot_entries_populate(app);
}

bool wlan_app_scene_loot_entries_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == LootEntryIdxDelete) {
        loot_show_delete_confirm(app);
        return true;
    }
    if(event.event == LootEntryIdxDelYes) {
        char session_dir[80];
        loot_session_path(app, session_dir, sizeof(session_dir));
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_simply_remove_recursive(storage, session_dir);
        furi_record_close(RECORD_STORAGE);
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    if(event.event == LootEntryIdxDelNo) {
        loot_entries_populate(app);
        return true;
    }
    if(event.event == LootEntryIdxEmpty) {
        return true; // non-actionable placeholder
    }
    if(s_entry_path && event.event < s_entry_count) {
        if(s_entry_path[event.event][0] != '\0') {
            strncpy(app->loot_file, s_entry_path[event.event], sizeof(app->loot_file) - 1);
            app->loot_file[sizeof(app->loot_file) - 1] = '\0';
            scene_manager_next_scene(app->scene_manager, WlanAppSceneLootPreview);
        }
        return true;
    }
    return false;
}

void wlan_app_scene_loot_entries_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
    free(s_entry_path);
    s_entry_path = NULL;
    s_entry_count = 0;
}
