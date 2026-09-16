#include "../subghz_i.h"

// Car Picker — step 1: pick a vehicle brand. Selecting one opens the model list
// (car_models scene), which applies that vehicle's capture frequency + modulation
// preset. Ported from the ProtoPirate app into the main SubGhz app (Car Keys hub).
// Data + helpers: ../car_presets.h.

#ifndef PUBLIC_BUILD
#include "../car_presets.h" // car make/model table — private only (guardrail)

#define CAR_PICKER_MAX_BRANDS 40

static void subghz_scene_car_picker_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_car_picker_on_enter(void* context) {
    SubGhz* subghz = context;

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "Pick Car Brand");

    const char* brands[CAR_PICKER_MAX_BRANDS];
    uint8_t n = car_presets_unique_brands(brands, CAR_PICKER_MAX_BRANDS);
    for(uint8_t i = 0; i < n; i++) {
        submenu_add_item(
            subghz->submenu, brands[i], i, subghz_scene_car_picker_submenu_callback, subghz);
    }

    submenu_set_selected_item(
        subghz->submenu,
        scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneCarPicker));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_car_picker_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        // event.event = selected brand index. Remember it (menu position) and hand it
        // to the models scene via that scene's state.
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneCarPicker, event.event);
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneCarModels, event.event);
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneCarModels);
        consumed = true;
    }
    return consumed;
}

void subghz_scene_car_picker_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}

#else /* PUBLIC_BUILD — car-only feature; provide empty handlers so the scene table links */

void subghz_scene_car_picker_on_enter(void* context) {
    UNUSED(context);
}
bool subghz_scene_car_picker_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}
void subghz_scene_car_picker_on_exit(void* context) {
    UNUSED(context);
}

#endif
