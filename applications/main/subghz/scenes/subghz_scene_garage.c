#include "../subghz_i.h"
#include <storage/storage.h>
#ifndef PUBLIC_BUILD
#include <flipper_format.h>
#include <lib/subghz/protocols/vag.h>
#endif

// "My Garage": a submenu of saved clone .subs under /ext/subghz/garage/.
// Selecting one loads it and re-opens the fob emulator (counter resumes).

#define GARAGE_PATH EXT_PATH("subghz/garage")
#define GARAGE_EXT  ".sub"

static void subghz_scene_garage_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_garage_on_enter(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "My Garage");

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    uint32_t count = 0;
    if(storage_dir_open(dir, GARAGE_PATH)) {
        FileInfo info;
        char name[128];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            char label[128];
            strncpy(label, name, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
            char* dot = strstr(label, GARAGE_EXT);
            if(dot) *dot = '\0'; // strip extension for display
            submenu_add_item(subghz->submenu, label, count, subghz_scene_garage_callback, subghz);
            count++;
            if(count >= 128) break;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    if(count == 0) {
        submenu_add_item(
            subghz->submenu, "(empty - Save a fob first)", 0xFFFF,
            subghz_scene_garage_callback, subghz);
    }

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneGarage));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_garage_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t sel = event.event;
        if(sel == 0xFFFF) return true; // (empty) placeholder
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneGarage, sel);

        // Resolve the sel-th .sub in the garage dir to a full path.
        Storage* storage = furi_record_open(RECORD_STORAGE);
        File* dir = storage_file_alloc(storage);
        FuriString* path = furi_string_alloc();
        bool found = false;
        if(storage_dir_open(dir, GARAGE_PATH)) {
            FileInfo info;
            char name[128];
            uint32_t i = 0;
            while(storage_dir_read(dir, &info, name, sizeof(name))) {
                if(info.flags & FSF_DIRECTORY) continue;
                if(i == sel) {
                    furi_string_printf(path, "%s/%s", GARAGE_PATH, name);
                    found = true;
                    break;
                }
                i++;
            }
        }
        storage_dir_close(dir);
        storage_file_free(dir);
        furi_record_close(RECORD_STORAGE);

        if(found && subghz_key_load(subghz, furi_string_get_cstr(path), false)) {
            furi_string_free(path);
#ifndef PUBLIC_BUILD
            // Clone entries (KeeLoq CloneMode=1 / VAG) resume in the Fob Emulator;
            // plain replay-saves (any other serializable fob) go to the Transmitter.
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            uint32_t clone_mode = 0;
            flipper_format_rewind(fff);
            flipper_format_read_uint32(fff, "CloneMode", &clone_mode, 1);
            FuriString* proto = furi_string_alloc();
            flipper_format_rewind(fff);
            bool is_vag = flipper_format_read_string(fff, "Protocol", proto) &&
                          furi_string_equal_str(proto, VAG_PROTOCOL_NAME);
            furi_string_free(proto);
            if(clone_mode == 1 || is_vag) {
                subghz->keeloq_fob_from_garage = true;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
            } else {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
            }
#else
            subghz->keeloq_fob_from_garage = true;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
#endif
            return true;
        }
        furi_string_free(path);
    }
    return false;
}

void subghz_scene_garage_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
