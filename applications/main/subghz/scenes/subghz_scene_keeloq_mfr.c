#include "../subghz_i.h" // IWYU pragma: keep
#include "../helpers/subghz_txrx_i.h"
#include "../helpers/subghz_keeloq_registry.h"
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/protocols/keeloq_common.h>

#define TAG "SubGhzKeeloqMfr"

// Stable storage for the picked manufacturer name. GenInfo.keeloq.manuf is a
// `const char*`, so it must point at memory that outlives this scene. The
// keystore string could otherwise be reloaded/freed, so we copy into a buffer
// with static storage duration.
static char subghz_keeloq_mfr_selected[64];

static SubGhzKeyArray_t* subghz_scene_keeloq_mfr_get_keys(SubGhz* subghz) {
    SubGhzEnvironment* environment = subghz->txrx->environment;
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    if(keystore == NULL) {
        return NULL;
    }
    return subghz_keystore_get_data(keystore);
}

void subghz_scene_keeloq_mfr_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;

    SubGhzKeyArray_t* data = subghz_scene_keeloq_mfr_get_keys(subghz);
    if(data == NULL) {
        return;
    }

    // Index refers to the FILTERED list (encodable manufacturers only), so we
    // only advance the counter for encodable entries. This keeps the submenu
    // index -> manufacturer mapping consistent with on_enter's filtered listing.
    uint32_t i = 0;
    const char* name = NULL;
    for
        M_EACH(key, *data, SubGhzKeyArray_t) {
            if(!subghz_keeloq_registry_type_is_generic_encodable(key->type)) {
                continue;
            }
            if(i == index) {
                name = furi_string_get_cstr(key->name);
                break;
            }
            i++;
        }

    if(name == NULL) {
        return;
    }

    // Copy into stable storage and point gen_info at it (avoids a dangling
    // pointer into keystore memory).
    strncpy(
        subghz_keeloq_mfr_selected, name, sizeof(subghz_keeloq_mfr_selected) - 1);
    subghz_keeloq_mfr_selected[sizeof(subghz_keeloq_mfr_selected) - 1] = '\0';
    subghz->gen_info->keeloq.manuf = subghz_keeloq_mfr_selected;

    // Mark that this generation came from the by-manufacturer picker, so the
    // generation-success path offers Emulate/Save instead of jumping to SaveName.
    subghz->keeloq_mfr_emulate_flow = true;

    // Enter the existing GenKeeloq serial/button/counter byte-input flow.
    scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetSerial);
}

void subghz_scene_keeloq_mfr_on_enter(void* context) {
    SubGhz* subghz = context;

    SubGhzKeyArray_t* data = subghz_scene_keeloq_mfr_get_keys(subghz);

    // Count only manufacturers the TX encoder can actually transmit. Listing a
    // non-encodable manufacturer would let the user pick one that silently
    // sends a zero/invalid frame.
    size_t encodable_count = 0;
    if(data != NULL) {
        for
            M_EACH(key, *data, SubGhzKeyArray_t) {
                if(subghz_keeloq_registry_type_is_generic_encodable(key->type)) {
                    encodable_count++;
                }
            }
    }

    if(encodable_count == 0) {
        widget_add_string_multiline_element(
            subghz->widget,
            64,
            32,
            AlignCenter,
            AlignCenter,
            FontSecondary,
            "No transmittable\nmanufacturers\nAdd keeloq_mfcodes_user\nto SD card");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "KeeLoq: pick manufacturer");

    uint32_t index = 0;
    for
        M_EACH(key, *data, SubGhzKeyArray_t) {
            if(!subghz_keeloq_registry_type_is_generic_encodable(key->type)) {
                continue;
            }
            submenu_add_item(
                subghz->submenu,
                furi_string_get_cstr(key->name),
                index,
                subghz_scene_keeloq_mfr_submenu_callback,
                subghz);
            index++;
        }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_keeloq_mfr_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_keeloq_mfr_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    widget_reset(subghz->widget);
}
