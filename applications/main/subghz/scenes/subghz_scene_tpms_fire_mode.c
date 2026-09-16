#include "../subghz_i.h"

#ifndef PUBLIC_BUILD
// TPMS Spoofer entry: ask Auto vs Manual fire BEFORE the band/mod scan setup.
//   Auto   -> a picked-up sensor auto-fires 0-PSI the instant it appears.
//   Manual -> the scan just lists sensors; the user opens one and fires it
//             (hold Left = 0-PSI, or Send to replay, or Save).
// The choice sets subghz->tpms_auto_fire_enabled, then drops into TpmsScanSetup.

typedef enum {
    TpmsFireMode_Auto,
    TpmsFireMode_Manual,
    TpmsFireMode_Survey,
} TpmsFireModeIndex;

static void subghz_scene_tpms_fire_mode_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_tpms_fire_mode_on_enter(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "TPMS Spoofer: fire mode");
    submenu_add_item(
        subghz->submenu,
        "Auto Fire (0-PSI on pickup)",
        TpmsFireMode_Auto,
        subghz_scene_tpms_fire_mode_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Manual (list, fire yourself)",
        TpmsFireMode_Manual,
        subghz_scene_tpms_fire_mode_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Survey (log all to CSV)",
        TpmsFireMode_Survey,
        subghz_scene_tpms_fire_mode_callback,
        subghz);
    submenu_set_selected_item(
        subghz->submenu, subghz->tpms_auto_fire_enabled ? TpmsFireMode_Auto : TpmsFireMode_Manual);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_tpms_fire_mode_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == TpmsFireMode_Auto) {
            subghz->tpms_auto_fire_enabled = true;
            subghz->tpms_survey_mode = false;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTpmsScanSetup);
            return true;
        } else if(event.event == TpmsFireMode_Manual) {
            subghz->tpms_auto_fire_enabled = false;
            subghz->tpms_survey_mode = false;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTpmsScanSetup);
            return true;
        } else if(event.event == TpmsFireMode_Survey) {
            // Survey: never fire, just log every decoded sensor to the CSV.
            subghz->tpms_auto_fire_enabled = false;
            subghz->tpms_survey_mode = true;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTpmsScanSetup);
            return true;
        }
    }
    return false;
}

void subghz_scene_tpms_fire_mode_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
#endif /* !PUBLIC_BUILD */
