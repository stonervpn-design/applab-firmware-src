#include "../subghz_i.h"
#include <storage/storage.h>

// "My Garage" = car-clone storage (saved clone/replay .subs) — PRIVATE builds
// only. The whole scene (incl. its car headers below) compiles to nothing in
// public; its ADD_SCENE + the only route to it (car_keys) are also gated.
#ifndef PUBLIC_BUILD
#include <flipper_format.h>
#include <lib/subghz/protocols/vag.h>

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

#ifndef PUBLIC_BUILD
        // Genie ".gne" next-code tables are not .sub key files -> open the Genie Sender.
        if(found) {
            const char* pstr = furi_string_get_cstr(path);
            size_t plen = strlen(pstr);
            if(plen >= 4 && strcmp(pstr + plen - 4, ".gne") == 0) {
                furi_string_set(subghz->file_path, path);
                furi_string_free(path);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneGenieSend);
                return true;
            }
        }
#endif

        if(found && subghz_key_load(subghz, furi_string_get_cstr(path), false)) {
            // Remember the source path so a clone opened from here persists its advancing
            // counter back to this same .sub after each transmit.
            furi_string_set(subghz->file_path, path);
            furi_string_free(path);
#ifndef PUBLIC_BUILD
            // Clone entries (KeeLoq CloneMode=1 / VAG) resume in the Fob Emulator;
            // a saved fob of any other SUPPORTED car protocol (Kia/Ford/Honda/...)
            // resumes in the same multi-button grid via the generic path; everything
            // else (plain replay-saves) goes to the Transmitter.
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            uint32_t clone_mode = 0;
            flipper_format_rewind(fff);
            flipper_format_read_uint32(fff, "CloneMode", &clone_mode, 1);
            FuriString* proto = furi_string_alloc();
            flipper_format_rewind(fff);
            bool have_proto = flipper_format_read_string(fff, "Protocol", proto);
            bool is_vag = have_proto && furi_string_equal_str(proto, VAG_PROTOCOL_NAME);
            if(clone_mode == 1 || is_vag) {
                subghz->keeloq_fob_from_garage = true;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
            } else if(have_proto && subghz_fob_can_emulate(fff, furi_string_get_cstr(proto))) {
                subghz->keeloq_fob_from_garage = true;
                subghz->keeloq_fob_generic = true;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
            } else {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
            }
            furi_string_free(proto);
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

#endif /* !PUBLIC_BUILD — car-clone storage, private only */
