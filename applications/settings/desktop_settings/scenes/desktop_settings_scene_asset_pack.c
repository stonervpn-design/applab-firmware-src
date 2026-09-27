#include "../desktop_settings_app.h"
#include "desktop_settings_scene.h"
#include "desktop_settings_scene_i.h"

#include <storage/storage.h>
#include <gui/asset_packs.h>
#include <momentum/momentum.h>

// Bound how many packs we list so the name table can't grow unbounded from a
// crafted SD card. 32 packs is far more than anyone keeps installed.
#define ASSET_PACK_LIST_MAX 32
#define ASSET_PACK_DEFAULT_INDEX 0

// The discovered pack directory names, index 1..pack_count (index 0 = Default).
// File-scope state is safe here: desktop_settings is a single-instance app and
// this list only lives between on_enter and on_exit of this one scene.
static char pack_names[ASSET_PACK_LIST_MAX][ASSET_PACKS_NAME_LEN];
static size_t pack_count;

static void desktop_settings_scene_asset_pack_submenu_callback(void* context, uint32_t index) {
    DesktopSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void desktop_settings_scene_asset_pack_on_enter(void* context) {
    DesktopSettingsApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, "Asset Pack");

    pack_count = 0;
    uint32_t preselect = ASSET_PACK_DEFAULT_INDEX;

    submenu_add_item(
        submenu,
        "Default (built-in)",
        ASSET_PACK_DEFAULT_INDEX,
        desktop_settings_scene_asset_pack_submenu_callback,
        app);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, ASSET_PACKS_PATH)) {
        FileInfo info;
        char name[256];
        while(pack_count < ASSET_PACK_LIST_MAX &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            // Only directories, and only names that fit the settings field so
            // what we store round-trips exactly to what asset_packs_init reads.
            if(!(info.flags & FSF_DIRECTORY)) continue;
            if(strlen(name) >= ASSET_PACKS_NAME_LEN) continue;

            strlcpy(pack_names[pack_count], name, ASSET_PACKS_NAME_LEN);
            uint32_t item_index = pack_count + 1;
            if(strcmp(name, momentum_settings.asset_pack) == 0) {
                preselect = item_index;
            }
            submenu_add_item(
                submenu,
                pack_names[pack_count],
                item_index,
                desktop_settings_scene_asset_pack_submenu_callback,
                app);
            pack_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    submenu_set_selected_item(submenu, preselect);
    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewMenu);
}

bool desktop_settings_scene_asset_pack_on_event(void* context, SceneManagerEvent event) {
    DesktopSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        const char* selected = "";
        if(event.event != ASSET_PACK_DEFAULT_INDEX) {
            size_t idx = event.event - 1;
            if(idx < pack_count) selected = pack_names[idx];
        }

        if(strcmp(selected, momentum_settings.asset_pack) != 0) {
            // Swap the pack out, persist the choice, then load the new one so
            // the change takes effect immediately (no reboot).
            asset_packs_free();
            strlcpy(momentum_settings.asset_pack, selected, ASSET_PACKS_NAME_LEN);
            momentum_settings_save();
            asset_packs_init();
        }

        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }
    return consumed;
}

void desktop_settings_scene_asset_pack_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    submenu_reset(app->submenu);
}
