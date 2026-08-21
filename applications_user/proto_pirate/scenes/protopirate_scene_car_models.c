// scenes/protopirate_scene_car_models.c
// Car Presets — step 2: pick a model for the brand chosen in car_picker, then apply that
// vehicle's capture frequency + modulation preset (AM650 = OOK, everything else = closest 2-FSK
// FM476) and return to the main menu. Data + helpers: ../car_presets.h.
#include "../protopirate_app_i.h"
#include "../car_presets.h"
#include "../helpers/protopirate_txrx.h"
#include "../helpers/protopirate_protocol_plugin_host.h"

#define CAR_MODELS_MAX_BRANDS 40
#define CAR_MODELS_MAX_MODELS 48

static void protopirate_scene_car_models_submenu_callback(void* context, uint32_t index) {
    furi_check(context);
    ProtoPirateApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

// The picker stored the selected brand index as this scene's state.
static const char* protopirate_scene_car_models_brand(ProtoPirateApp* app) {
    const char* brands[CAR_MODELS_MAX_BRANDS];
    uint8_t n = car_presets_unique_brands(brands, CAR_MODELS_MAX_BRANDS);
    uint32_t idx = scene_manager_get_scene_state(app->scene_manager, ProtoPirateSceneCarModels);
    if(n == 0) return "";
    if(idx >= n) idx = 0;
    return brands[idx];
}

// Map a car_presets modulation tag to a Flipper preset name. AM650 stays OOK; the FSK variants
// (FM476/F3/F4/Honda1) map to the closest standard 2-FSK preset the setting is guaranteed to have.
static const char* protopirate_scene_car_models_preset_name(const char* mod) {
    if(strcmp(mod, "AM650") == 0) return "AM650";
    return "FM476";
}

void protopirate_scene_car_models_on_enter(void* context) {
    furi_check(context);
    ProtoPirateApp* app = context;
    const char* brand = protopirate_scene_car_models_brand(app);

    uint8_t shown = 0;
    char label[64];
    for(uint8_t i = 0; CAR_PRESETS[i].brand && shown < CAR_MODELS_MAX_MODELS; i++) {
        if(strcmp(CAR_PRESETS[i].brand, brand) != 0) continue;
        const CarPreset* p = &CAR_PRESETS[i];
        // e.g. "Tucson '09-'19 433F"  (F = FSK, A = OOK/AM)
        snprintf(
            label,
            sizeof(label),
            "%s '%02u-'%02u %lu%c",
            p->model,
            (unsigned)(p->year_start % 100),
            (unsigned)(p->year_end % 100),
            (unsigned long)(p->frequency / 1000000),
            (strcmp(p->mod, "AM650") == 0) ? 'A' : 'F');
        // The submenu item id is the CAR_PRESETS table index, so on_event recovers the row directly.
        submenu_add_item(
            app->submenu, label, i, protopirate_scene_car_models_submenu_callback, app);
        shown++;
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, ProtoPirateViewSubmenu);
}

bool protopirate_scene_car_models_on_event(void* context, SceneManagerEvent event) {
    furi_check(context);
    ProtoPirateApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        const CarPreset* p = &CAR_PRESETS[event.event];
        const char* pname = protopirate_scene_car_models_preset_name(p->mod);

        // Locate the matching preset in the SubGhz setting.
        uint8_t pidx = 0;
        for(uint8_t i = 0; i < subghz_setting_get_preset_count(app->setting); i++) {
            if(strcmp(subghz_setting_get_preset_name(app->setting, i), pname) == 0) {
                pidx = i;
                break;
            }
        }

        // Apply frequency + modulation preset, then rebuild the protocol registry for it.
        app->txrx->preset->frequency = p->frequency;
        protopirate_preset_init(
            app,
            subghz_setting_get_preset_name(app->setting, pidx),
            p->frequency,
            subghz_setting_get_preset_data(app->setting, pidx),
            subghz_setting_get_preset_data_size(app->setting, pidx));

        if(protopirate_refresh_protocol_registry(app, false)) {
            notification_message(app->notifications, &sequence_success);
        } else {
            notification_message(app->notifications, &sequence_error);
        }

        // Settings applied — jump back to the main menu (the picker/models scenes fall off the stack).
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, ProtoPirateSceneStart);
        consumed = true;
    }
    return consumed;
}

void protopirate_scene_car_models_on_exit(void* context) {
    furi_check(context);
    ProtoPirateApp* app = context;
    submenu_reset(app->submenu);
}
