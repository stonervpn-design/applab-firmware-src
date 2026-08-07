#include "../subghz_i.h"

#define TAG "SubGhzSceneSpectrum"

// Swept-RSSI panadapter scene (PRIVATE). Lazy-loads the spectrum view only while
// active (no-PSRAM heap discipline). The sweep worker + input handling are wired
// in later tasks.

static void subghz_scene_spectrum_view_cb(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_spectrum_on_enter(void* context) {
    SubGhz* subghz = context;

    if(!subghz->subghz_spectrum) {
        subghz->subghz_spectrum = subghz_view_spectrum_alloc();
        view_dispatcher_add_view(
            subghz->view_dispatcher,
            SubGhzViewIdSpectrum,
            subghz_view_spectrum_get_view(subghz->subghz_spectrum));
    }

    subghz_view_spectrum_set_callback(
        subghz->subghz_spectrum, subghz_scene_spectrum_view_cb, subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdSpectrum);
}

bool subghz_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    // Custom-event handling (band shift, tune-to-peak) is added in a later task.
    return false;
}

void subghz_scene_spectrum_on_exit(void* context) {
    SubGhz* subghz = context;
    if(subghz->subghz_spectrum) {
        view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdSpectrum);
        subghz_view_spectrum_free(subghz->subghz_spectrum);
        subghz->subghz_spectrum = NULL;
    }
}
