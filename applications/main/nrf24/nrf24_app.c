#include "nrf24_app.h"
#include "nrf24_hw.h"
#include <furi_hal_power.h>
#include <stdlib.h>
#ifndef PUBLIC_BUILD
#include "helpers/nrf24_capture.h" // on-demand capture ring, freed here (private only)
#endif

static bool nrf24_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Nrf24App* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool nrf24_app_back_event_callback(void* context) {
    furi_assert(context);
    Nrf24App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void nrf24_app_tick_event_callback(void* context) {
    furi_assert(context);
    Nrf24App* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static Nrf24App* nrf24_app_alloc(void) {
    Nrf24App* app = malloc(sizeof(Nrf24App));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    if(!app) return NULL;

    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->scene_manager = scene_manager_alloc(&nrf24_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nrf24_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, nrf24_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, nrf24_app_tick_event_callback, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewWidget, widget_get_view(app->widget));

    app->spectrum_view = nrf24_spectrum_view_alloc();
    view_set_context(app->spectrum_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewSpectrum, app->spectrum_view);

    app->jam_view = nrf24_jam_view_alloc();
    view_set_context(app->jam_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewJam, app->jam_view);

    app->mj_scan_view = nrf24_mj_scan_view_alloc();
    view_set_context(app->mj_scan_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewMjScan, app->mj_scan_view);

    app->mj_attack_view = nrf24_mj_attack_view_alloc();
    view_set_context(app->mj_attack_view, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, Nrf24ViewMjAttack, app->mj_attack_view);

    app->mj_target_count = 0;
    app->mj_selected_target = -1;
    app->mj_script_path = furi_string_alloc();
    app->mj_auto_mode = false;

    return app;
}

static void nrf24_app_free(Nrf24App* app) {
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewSpectrum);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewJam);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewMjScan);
    view_dispatcher_remove_view(app->view_dispatcher, Nrf24ViewMjAttack);

    submenu_free(app->submenu);
    widget_free(app->widget);
    nrf24_spectrum_view_free(app->spectrum_view);
    nrf24_jam_view_free(app->jam_view);
    nrf24_mj_scan_view_free(app->mj_scan_view);
    nrf24_mj_attack_view_free(app->mj_attack_view);

    if(app->mj_script_path) {
        furi_string_free(app->mj_script_path);
        app->mj_script_path = NULL;
    }

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

#ifndef PUBLIC_BUILD
    /* Return the on-demand capture ring (~3 KB) to the heap. Safe here: every
     * scene's on_exit (which stops its RX/promiscuous worker) has already run
     * via scene_manager_free above, so no capture writer can race this. */
    nrf24_capture_free();
#endif

    furi_record_close(RECORD_STORAGE);
    app->storage = NULL;
    furi_record_close(RECORD_DIALOGS);
    app->dialogs = NULL;
    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    free(app);
}

int32_t nrf24_app(void* args) {
    UNUSED(args);

    Nrf24App* app = nrf24_app_alloc();

    /* Probe the NRF24 up front and show a clear "not available" message instead
     * of opening a menu whose every action silently fails. Consistent with the
     * SubGHz/NFC apps. The probe is self-contained (init/acquire/probe/clean up);
     * the mode scenes re-init the radio themselves when the module is present. */
    nrf24_hw_init();
    nrf24_hw_acquire();
    bool nrf24_present = nrf24_hw_probe();
    nrf24_hw_release();
    nrf24_hw_deinit();

    if(!nrf24_present) {
        FURI_LOG_W("Nrf24", "NRF24 not detected, aborting app launch");
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "NRF24 Not Available", 64, 8, AlignCenter, AlignTop);
        dialog_message_set_text(
            message,
            "No NRF24 module detected\non the SPI bus.",
            64,
            34,
            AlignCenter,
            AlignCenter);
        dialog_message_set_buttons(message, NULL, NULL, "OK");
        dialog_message_show(app->dialogs, message);
        dialog_message_free(message);
        nrf24_app_free(app);
        return 0;
    }

    /* Hold the CPU at max for the whole nRF24 session: the jammer's channel-hop
     * loop is CPU-timing-critical, and under DFS the CPU would otherwise sit at the
     * min frequency even while busy-looping (esp_pm sets freq by lock, not load). */
    furi_hal_power_perf_lock_acquire();
    scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMenu);
    view_dispatcher_run(app->view_dispatcher);
    furi_hal_power_perf_lock_release();

    nrf24_app_free(app);
    return 0;
}
