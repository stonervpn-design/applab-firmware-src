#include "../subghz_i.h"

#ifndef PUBLIC_BUILD
// TPMS Scan Setup — pick the band + modulation before the TPMS Spoofer scan.
// Real TPMS sensors span two bands (315 MHz US/Asia, 433.92 MHz EU) and both
// modulations (2-FSK at various deviations, and OOK/ASK). The old scan hard-coded
// 433.92 MHz / FM238, so it could not hear 315 MHz or OOK sensors at all. This
// submenu stores the chosen freq + preset (subghz->tpms_scan_freq /
// tpms_scan_preset_idx), arms tpms_scan_mode, and drops into the receiver, which
// honors the choice.

typedef enum {
    TpmsScanSetup_433_FM238,
    TpmsScanSetup_315_FM238,
    TpmsScanSetup_433_FM476,
    TpmsScanSetup_315_FM476,
    TpmsScanSetup_433_AM650,
    TpmsScanSetup_315_AM650,
} TpmsScanSetupIndex;

static void subghz_scene_tpms_scan_setup_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_tpms_scan_setup_on_enter(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "TPMS Scan: band / mod");
    submenu_add_item(
        subghz->submenu,
        "433.92 MHz FSK (EU)",
        TpmsScanSetup_433_FM238,
        subghz_scene_tpms_scan_setup_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "315 MHz FSK (US/Asia)",
        TpmsScanSetup_315_FM238,
        subghz_scene_tpms_scan_setup_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "433.92 MHz FSK-wide",
        TpmsScanSetup_433_FM476,
        subghz_scene_tpms_scan_setup_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "315 MHz FSK-wide",
        TpmsScanSetup_315_FM476,
        subghz_scene_tpms_scan_setup_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "433.92 MHz OOK",
        TpmsScanSetup_433_AM650,
        subghz_scene_tpms_scan_setup_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "315 MHz OOK",
        TpmsScanSetup_315_AM650,
        subghz_scene_tpms_scan_setup_callback,
        subghz);

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneTpmsScanSetup));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

static void subghz_scene_tpms_scan_setup_start(SubGhz* subghz, uint32_t freq, const char* preset) {
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    int idx = subghz_setting_get_inx_preset_by_name(setting, preset);
    subghz->tpms_scan_freq = freq;
    subghz->tpms_scan_preset_idx = (uint8_t)(idx >= 0 ? idx : 0);
    subghz->tpms_scan_mode = true;
    subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
    scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
}

bool subghz_scene_tpms_scan_setup_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneTpmsScanSetup, event.event);
        switch(event.event) {
        case TpmsScanSetup_433_FM238:
            // "FM238" name kept in the enum, but the real TPMS-tuned preset (19.2
            // kBaud / 28.6 kHz) is used — stock FM238's 4.8 kBaud decodes nothing.
            subghz_scene_tpms_scan_setup_start(subghz, 433920000, "TPMS");
            return true;
        case TpmsScanSetup_315_FM238:
            subghz_scene_tpms_scan_setup_start(subghz, 315000000, "TPMS");
            return true;
        case TpmsScanSetup_433_FM476:
            subghz_scene_tpms_scan_setup_start(subghz, 433920000, "TPMS");
            return true;
        case TpmsScanSetup_315_FM476:
            subghz_scene_tpms_scan_setup_start(subghz, 315000000, "TPMS");
            return true;
        case TpmsScanSetup_433_AM650:
            subghz_scene_tpms_scan_setup_start(subghz, 433920000, "AM650");
            return true;
        case TpmsScanSetup_315_AM650:
            subghz_scene_tpms_scan_setup_start(subghz, 315000000, "AM650");
            return true;
        default:
            break;
        }
    }
    return false;
}

void subghz_scene_tpms_scan_setup_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
#endif /* !PUBLIC_BUILD — TPMS scan setup is part of the private TPMS Spoofer */
