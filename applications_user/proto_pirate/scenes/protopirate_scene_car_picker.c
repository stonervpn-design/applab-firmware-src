// scenes/protopirate_scene_car_picker.c
// Car Presets — step 1: pick a brand. Selecting one opens the model list (car_models scene),
// which applies that vehicle's capture frequency + modulation. Data: ../car_presets.h.
#include "../protopirate_app_i.h"
#include "../car_presets.h"

#define CAR_PICKER_MAX_BRANDS 40

static void protopirate_scene_car_picker_submenu_callback(void* context, uint32_t index) {
    furi_check(context);
    ProtoPirateApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void protopirate_scene_car_picker_on_enter(void* context) {
    furi_check(context);
    ProtoPirateApp* app = context;

    const char* brands[CAR_PICKER_MAX_BRANDS];
    uint8_t n = car_presets_unique_brands(brands, CAR_PICKER_MAX_BRANDS);
    for(uint8_t i = 0; i < n; i++) {
        submenu_add_item(
            app->submenu, brands[i], i, protopirate_scene_car_picker_submenu_callback, app);
    }

    submenu_set_selected_item(
        app->submenu,
        scene_manager_get_scene_state(app->scene_manager, ProtoPirateSceneCarPicker));
    view_dispatcher_switch_to_view(app->view_dispatcher, ProtoPirateViewSubmenu);
}

bool protopirate_scene_car_picker_on_event(void* context, SceneManagerEvent event) {
    furi_check(context);
    ProtoPirateApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        // event.event = selected brand index. Remember it for this scene (menu position) and
        // hand it to the models scene via that scene's state.
        scene_manager_set_scene_state(app->scene_manager, ProtoPirateSceneCarPicker, event.event);
        scene_manager_set_scene_state(app->scene_manager, ProtoPirateSceneCarModels, event.event);
        scene_manager_next_scene(app->scene_manager, ProtoPirateSceneCarModels);
        consumed = true;
    }
    return consumed;
}

void protopirate_scene_car_picker_on_exit(void* context) {
    furi_check(context);
    ProtoPirateApp* app = context;
    submenu_reset(app->submenu);
}
