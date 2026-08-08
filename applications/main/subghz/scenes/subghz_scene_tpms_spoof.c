#include "../subghz_i.h"

// TPMS Spoofer (PRIVATE-only): pick a protocol -> seed a synthetic sensor frame
// from scratch (no capture needed) -> hand it to the Transmitter for hold-OK
// burst TX. Only protocols with a field->data packer are offered (Schrader GG4
// today); the others need packers written first.
#ifndef PUBLIC_BUILD

#include "../helpers/tpms_spoof.h"

static void subghz_scene_tpms_spoof_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_tpms_spoof_on_enter(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "TPMS Spoofer");

    for(size_t i = 0; i < tpms_spoof_proto_count(); i++) {
        const TpmsSpoofProto* p = tpms_spoof_proto_get(i);
        if(p) submenu_add_item(
            subghz->submenu, p->label, (uint32_t)i, subghz_scene_tpms_spoof_callback, subghz);
    }

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneTpmsSpoof));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_tpms_spoof_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t idx = event.event;
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneTpmsSpoof, idx);
        if(tpms_spoof_seed(subghz, idx)) {
            // The synthetic frame is loaded into the radio; the Transmitter scene
            // sends it (tap = one burst, hold OK = continuous). TX is user-gated.
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        } else {
            furi_string_set(subghz->error_str, "TPMS spoof seed failed.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        }
        return true;
    }
    return false;
}

void subghz_scene_tpms_spoof_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}

#endif // PUBLIC_BUILD
