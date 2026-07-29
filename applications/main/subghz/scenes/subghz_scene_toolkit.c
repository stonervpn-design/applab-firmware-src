#include "../subghz_i.h"
#include "../helpers/subghz_txrx_i.h"
#include "../helpers/subghz_custom_event.h"

#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>

#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>

#define TAG "SubGhzToolkit"

#define SUBGHZ_TOOLKIT_KEYSTORE_PATH EXT_PATH("subghz/analysis/keeloq_keys.txt")

// Read-only tool. The enclave-decrypt path from the original RocketGods toolkit
// is intentionally dropped (furi_hal_crypto_decrypt is a stub on this port);
// we only dump the already-loaded plaintext keystore.

typedef enum {
    SubGhzSceneToolkitIndexProtocols,
    SubGhzSceneToolkitIndexExportKeystore,
} SubGhzSceneToolkitIndex;

// Scene state: which view is currently shown (used to route Back from popup).
typedef enum {
    SubGhzSceneToolkitStateMenu = 0,
    SubGhzSceneToolkitStatePopup = 1,
} SubGhzSceneToolkitState;

static bool subghz_scene_toolkit_export_keystore(SubGhz* subghz) {
    SubGhzEnvironment* environment = subghz->txrx->environment;
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    if(keystore == NULL) {
        FURI_LOG_E(TAG, "No keystore available");
        return false;
    }

    bool success = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);

    do {
        storage_simply_mkdir(storage, EXT_PATH("subghz"));
        storage_simply_mkdir(storage, EXT_PATH("subghz/analysis"));

        if(!file_stream_open(
               stream, SUBGHZ_TOOLKIT_KEYSTORE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            FURI_LOG_E(TAG, "Failed to open keystore output file");
            break;
        }

        stream_write_format(
            stream,
            "====================================\n"
            "  Flipper SubGhz KeeLoq Mfcodes\n"
            "  Exported by SubGhz Toolkit\n"
            "====================================\n\n");

        SubGhzKeyArray_t* keys = subghz_keystore_get_data(keystore);
        size_t key_count = SubGhzKeyArray_size(*keys);

        stream_write_format(stream, "Total Keys: %zu\n\n", key_count);

        for
            M_EACH(key, *keys, SubGhzKeyArray_t) {
                stream_write_format(
                    stream,
                    "Manufacturer: %s\n"
                    "Key (Hex): %016llX\n"
                    "Type: %hu\n\n",
                    furi_string_get_cstr(key->name),
                    key->key,
                    key->type);
            }

        success = true;
    } while(0);

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    return success;
}

static void subghz_scene_toolkit_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

static void subghz_scene_toolkit_popup_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventSceneExit);
}

void subghz_scene_toolkit_on_enter(void* context) {
    SubGhz* subghz = context;

    scene_manager_set_scene_state(
        subghz->scene_manager, SubGhzSceneToolkit, SubGhzSceneToolkitStateMenu);

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "SubGhz Toolkit");

    submenu_add_item(
        subghz->submenu,
        "Protocol Analysis",
        SubGhzSceneToolkitIndexProtocols,
        subghz_scene_toolkit_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Export Keystore",
        SubGhzSceneToolkitIndexExportKeystore,
        subghz_scene_toolkit_submenu_callback,
        subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_toolkit_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeBack) {
        // While the popup is up, Back returns to this submenu instead of
        // popping the whole scene.
        if(scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneToolkit) ==
           SubGhzSceneToolkitStatePopup) {
            popup_reset(subghz->popup);
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneToolkit, SubGhzSceneToolkitStateMenu);
            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
            return true;
        }
        return false;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzSceneToolkitIndexProtocols) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneToolkitProtocols);
            return true;
        } else if(event.event == SubGhzSceneToolkitIndexExportKeystore) {
            bool ok = subghz_scene_toolkit_export_keystore(subghz);

            Popup* popup = subghz->popup;
            popup_reset(popup);
            if(ok) {
                popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup,
                    "Keys saved to:\n/ext/subghz/analysis/\nkeeloq_keys.txt",
                    64,
                    24,
                    AlignCenter,
                    AlignTop);
            } else {
                popup_set_header(popup, "Error!", 64, 10, AlignCenter, AlignTop);
                popup_set_text(popup, "Failed to export keys", 64, 24, AlignCenter, AlignTop);
            }
            popup_set_context(popup, subghz);
            popup_set_callback(popup, subghz_scene_toolkit_popup_callback);
            popup_set_timeout(popup, 3000);
            popup_enable_timeout(popup);

            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneToolkit, SubGhzSceneToolkitStatePopup);
            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);
            return true;
        } else if(event.event == SubGhzCustomEventSceneExit) {
            // Popup timed out -> return to the toolkit submenu.
            popup_reset(subghz->popup);
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneToolkit, SubGhzSceneToolkitStateMenu);
            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
            return true;
        }
    }

    return false;
}

void subghz_scene_toolkit_on_exit(void* context) {
    SubGhz* subghz = context;
    popup_reset(subghz->popup);
    submenu_reset(subghz->submenu);
}
