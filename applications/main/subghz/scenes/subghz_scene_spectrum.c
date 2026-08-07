#include "../subghz_i.h"
#include "../helpers/subghz_spectrum_worker.h"

#define TAG "SubGhzSceneSpectrum"

// Sweep worker for the active scene (single instance, alloc/free on enter/exit).
static SubGhzSpectrumWorker* s_spectrum_worker = NULL;

// Swept-RSSI panadapter scene (PRIVATE). Lazy-loads the spectrum view only while
// active (no-PSRAM heap discipline). The sweep worker + input handling are wired
// in later tasks.

static void subghz_scene_spectrum_view_cb(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

// Runs on the worker thread; push_row is model-locked so this is cross-thread safe.
static void subghz_scene_spectrum_row_cb(
    void* ctx,
    uint32_t band_start,
    uint32_t bin_step,
    const uint8_t* bins,
    size_t n,
    uint32_t peak_freq,
    float peak_rssi) {
    SubGhz* subghz = ctx;
    if(subghz->subghz_spectrum) {
        subghz_view_spectrum_push_row(
            subghz->subghz_spectrum, band_start, bin_step, bins, n, peak_freq, peak_rssi);
    }
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

    if(!s_spectrum_worker) s_spectrum_worker = subghz_spectrum_worker_alloc();
    subghz_spectrum_worker_set_row_callback(
        s_spectrum_worker, subghz_scene_spectrum_row_cb, subghz);
    subghz_spectrum_worker_start(s_spectrum_worker);

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

    // Stop + join the worker FIRST so no row callback can touch the view we free.
    if(s_spectrum_worker) {
        if(subghz_spectrum_worker_is_running(s_spectrum_worker)) {
            subghz_spectrum_worker_stop(s_spectrum_worker);
        }
        subghz_spectrum_worker_free(s_spectrum_worker);
        s_spectrum_worker = NULL;
    }

    if(subghz->subghz_spectrum) {
        view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdSpectrum);
        subghz_view_spectrum_free(subghz->subghz_spectrum);
        subghz->subghz_spectrum = NULL;
    }
}
