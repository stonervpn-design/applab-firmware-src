#include "../subghz_i.h"
#include <lib/subghz/subghz_setting.h>

// Car Picker — step 2: pick a model for the brand chosen in car_picker, then apply
// that vehicle's capture frequency + modulation preset (AM650 = OOK; the FSK variants
// map to the closest standard 2-FSK FM476 preset), enable the Cars decoder set, and
// drop into the receiver so the fob is captured on the correct freq/modulation.
// Ported from the ProtoPirate app. Data + helpers: ../car_presets.h.

#ifndef PUBLIC_BUILD
#include "../car_presets.h" // car make/model table — private only (guardrail)

#define CAR_MODELS_MAX_BRANDS 40
#define CAR_MODELS_MAX_MODELS 48

static void subghz_scene_car_models_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

// The picker stored the selected brand index as this scene's state.
static const char* subghz_scene_car_models_brand(SubGhz* subghz) {
    const char* brands[CAR_MODELS_MAX_BRANDS];
    uint8_t n = car_presets_unique_brands(brands, CAR_MODELS_MAX_BRANDS);
    uint32_t idx = scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneCarModels);
    if(n == 0) return "";
    if(idx >= n) idx = 0;
    return brands[idx];
}

// Map a car_presets modulation tag to a Flipper preset name the setting is guaranteed
// to have. AM650 stays OOK; every FSK variant (FM476/F3/F4/Honda1) maps to FM476.
static const char* subghz_scene_car_models_preset_name(const char* mod) {
    if(strcmp(mod, "AM650") == 0) return "AM650";
    return "FM476";
}

void subghz_scene_car_models_on_enter(void* context) {
    SubGhz* subghz = context;
    const char* brand = subghz_scene_car_models_brand(subghz);

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, brand);

    char label[64];
    uint8_t shown = 0;
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
        // The submenu item id is the CAR_PRESETS table index, so on_event recovers the row.
        submenu_add_item(
            subghz->submenu, label, i, subghz_scene_car_models_submenu_callback, subghz);
        shown++;
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_car_models_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        const CarPreset* p = &CAR_PRESETS[event.event];
        const char* pname = subghz_scene_car_models_preset_name(p->mod);

        // Resolve the modulation preset index in the SubGhz setting.
        SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
        int pidx = subghz_setting_get_inx_preset_by_name(setting, pname);
        if(pidx < 0) pidx = 0;

        // The receiver re-applies frequency + preset from last_settings on enter (when
        // rx_key_state == IDLE), so a plain subghz_txrx_set_preset() here gets clobbered
        // and the modulation never changes. Write the car's values into last_settings —
        // the channel the receiver actually reads — then apply immediately.
        subghz->last_settings->frequency = p->frequency;
        subghz->last_settings->preset_index = (uint8_t)pidx;
        subghz_txrx_set_preset_internal(
            subghz->txrx, p->frequency, (uint8_t)pidx, subghz->last_settings->tx_power);

        // Enable the Cars decoder set so the captured fob decodes (Car Keys restores the
        // prior mode on exit), then drop into the receiver on the chosen freq/modulation.
        subghz_txrx_set_car_mode(subghz->txrx, 1);
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
        notification_message(subghz->notifications, &sequence_success);
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
        consumed = true;
    }
    return consumed;
}

void subghz_scene_car_models_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}

#else /* PUBLIC_BUILD — car-only feature; empty handlers so the scene table links */

void subghz_scene_car_models_on_enter(void* context) {
    UNUSED(context);
}
bool subghz_scene_car_models_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}
void subghz_scene_car_models_on_exit(void* context) {
    UNUSED(context);
}

#endif
