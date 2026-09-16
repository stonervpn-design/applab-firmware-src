#include "../subghz_i.h"

#ifndef PUBLIC_BUILD
// Unified "Car Keys" hub: groups the scattered car/RKE features (capture with
// Cars decoders on, saved-fob Garage, the vuln browser, RollBack, Toolkit) into
// one entry. New Capture auto-enables the Cars decoder set; the prior car_mode
// is restored on exit so it doesn't leak into normal Read.

typedef enum {
    SubGhzSceneCarKeysNewCapture,
    SubGhzSceneCarKeysCarPicker,
    SubGhzSceneCarKeysGarage,
    SubGhzSceneCarKeysVulns,
    SubGhzSceneCarKeysRollBack,
    SubGhzSceneCarKeysToolkit,
    SubGhzSceneCarKeysKeeloqConsole,
    SubGhzSceneCarKeysHitag2Crack,
    SubGhzSceneCarKeysGenieRecord,
    SubGhzSceneCarKeysVulnScan,
    SubGhzSceneCarKeysVulnReport,
} SubGhzSceneCarKeysIndex;

static void subghz_scene_car_keys_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_car_keys_on_enter(void* context) {
    SubGhz* subghz = context;

    // Remember the prior automotive mode so we can restore it on exit.
    subghz->car_keys_prev_car_mode = subghz_txrx_get_car_mode(subghz->txrx);

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "Car Keys");
    submenu_add_item(
        subghz->submenu,
        "New Capture",
        SubGhzSceneCarKeysNewCapture,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Car Picker (freq/mod)",
        SubGhzSceneCarKeysCarPicker,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "My Garage",
        SubGhzSceneCarKeysGarage,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Vehicle Vulns",
        SubGhzSceneCarKeysVulns,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Vuln Scan (assess)",
        SubGhzSceneCarKeysVulnScan,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Vuln Report",
        SubGhzSceneCarKeysVulnReport,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "RollBack",
        SubGhzSceneCarKeysRollBack,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Toolkit",
        SubGhzSceneCarKeysToolkit,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "KeeLoq Console (all keys)",
        SubGhzSceneCarKeysKeeloqConsole,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "HITAG2 Crack (dict)",
        SubGhzSceneCarKeysHitag2Crack,
        subghz_scene_car_keys_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Genie Recorder",
        SubGhzSceneCarKeysGenieRecord,
        subghz_scene_car_keys_submenu_callback,
        subghz);

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneCarKeys));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_car_keys_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneCarKeys, event.event);
        switch(event.event) {
        case SubGhzSceneCarKeysNewCapture:
            // Auto-enable the Cars decoder set, then drop into the receiver.
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            subghz_txrx_set_car_mode(subghz->txrx, 1);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        case SubGhzSceneCarKeysCarPicker:
            // Pick make/model -> set the correct capture frequency + modulation.
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneCarPicker);
            return true;
        case SubGhzSceneCarKeysGarage:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneGarage);
            return true;
        case SubGhzSceneCarKeysVulns:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnMakes);
            return true;
        case SubGhzSceneCarKeysVulnScan:
            // Capture-then-assess: car decoders on + vuln_scan_mode so opening a
            // captured signal routes to Vuln Assess instead of the info view.
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            subghz_txrx_set_car_mode(subghz->txrx, 1);
            subghz->vuln_scan_mode = true;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        case SubGhzSceneCarKeysVulnReport:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnReport);
            return true;
        case SubGhzSceneCarKeysRollBack:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollbackCapture);
            return true;
        case SubGhzSceneCarKeysToolkit:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneToolkit);
            return true;
        case SubGhzSceneCarKeysKeeloqConsole:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqConsole);
            return true;
        case SubGhzSceneCarKeysHitag2Crack:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneHitag2Crack);
            return true;
        case SubGhzSceneCarKeysGenieRecord:
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneGenieRecord);
            return true;
        default:
            break;
        }
    }
    return false;
}

void subghz_scene_car_keys_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    // Restore the automotive mode the user had before entering Car Keys.
    subghz_txrx_set_car_mode(subghz->txrx, subghz->car_keys_prev_car_mode);
}
#endif /* !PUBLIC_BUILD — Car Keys hub is car-only, stripped from public */
