#include "../subghz_i.h"

#define TAG "SubGhzKeeloqAction"

typedef enum {
    SubGhzSceneKeeloqActionEmulate = 0,
    SubGhzSceneKeeloqActionSave = 1,
} SubGhzSceneKeeloqActionIndex;

void subghz_scene_keeloq_action_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;

    switch(index) {
    case SubGhzSceneKeeloqActionEmulate:
        // Route straight into the existing Transmitter on the generated (unsaved)
        // fob data. The Transmitter auto-advances the rolling counter and maps the
        // direction keys to different button commands.
        subghz->keeloq_mfr_emulate_flow = false;
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        break;
    case SubGhzSceneKeeloqActionSave:
        // Same behaviour the by-manufacturer flow had before this action scene:
        // clear the temp name and hand off to SaveName.
        subghz->keeloq_mfr_emulate_flow = false;
        subghz_file_name_clear(subghz);
        scene_manager_set_scene_state(
            subghz->scene_manager, SubGhzSceneSetType, SubGhzCustomEventManagerSet);
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);
        break;
    default:
        break;
    }
}

void subghz_scene_keeloq_action_on_enter(void* context) {
    SubGhz* subghz = context;

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "KeeLoq Fob");

    submenu_add_item(
        subghz->submenu,
        "Emulate",
        SubGhzSceneKeeloqActionEmulate,
        subghz_scene_keeloq_action_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Save",
        SubGhzSceneKeeloqActionSave,
        subghz_scene_keeloq_action_submenu_callback,
        subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_keeloq_action_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_keeloq_action_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    // Clear on any exit (incl. Back) so a back-out never leaves the flow armed.
    subghz->keeloq_mfr_emulate_flow = false;
}
