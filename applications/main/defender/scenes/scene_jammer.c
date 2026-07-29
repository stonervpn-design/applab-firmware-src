#include "../defender_app_i.h"
#include "../defender_jammer.h"
#include "../../subghz/helpers/subghz_threshold_rssi.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>
#include <subghz/devices/devices.h>
#include <furi_hal_subghz.h>
#include <furi_hal_power.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>

/**
 * Sub-GHz jamming / RF-energy monitor scene.
 *
 * Parks the CC1101 at 433.92 MHz and samples RSSI every ~50 ms from a FuriTimer,
 * feeding the pure floor-EMA state machine (defender_jammer_feed) and scrolling a
 * waterfall energy bar. Phase-1 RSSI-only fallback: packet_decoded is always
 * false (the decoder exposes no packet-complete hook), per the locked decision.
 *
 * Threading: the 50 ms timer thread owns all radio + jammer-state access and the
 * waterfall push. On a rising jam edge it only sets a volatile flag + snapshot;
 * the scene-manager tick (dispatcher thread) raises the debounced alert, so the
 * shared alert bus is only ever touched from the dispatcher thread (same thread
 * as defender_alert_tick).
 *
 * Radio discipline: subghz_devices begin/reset on enter behind a runtime CC1101
 * probe (subghz_devices_is_connect); on exit the timer is stopped first, then the
 * radio is idled/slept/released and the RSSI reader freed. If no CC1101 is
 * present a "No Sub-GHz" notice is shown and the scene backs out (not board-gated).
 */

#define JAMMER_OK_LOCATE 230u
#define JAMMER_NO_RADIO  231u
#define JAMMER_FREQ      433920000u
// This board's CC1101 sits on the external 14-pin bus (driven by the cc1101_ext
// driver, powered from the 5V OTG boost) — same device the main Sub-GHz app
// uses. The old "cc1101_int" (Flipper-Zero built-in driver) never probes here.
#define JAMMER_DEVICE    SUBGHZ_DEVICE_CC1101_EXT_NAME
#define JAMMER_BINS      32
#define JAM_RSSI_FLOOR   (-110)
#define JAM_RSSI_CEIL    (-40)

static DefenderApp* s_app;
static const SubGhzDevice* s_device;
static SubGhzThresholdRssi* s_rssi;
static FuriTimer* s_timer;
static bool s_inited;
static bool s_began;
static bool s_running;
static DefenderJammerState s_jam;

static volatile bool s_jam_edge;
static volatile int8_t s_jam_rssi;
static volatile int8_t s_jam_over_db;

static bool defender_jammer_ok_input(InputEvent* event, void* context) {
    UNUSED(context);
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event(s_app->view_dispatcher, JAMMER_OK_LOCATE);
        return true;
    }
    return false;
}

static void defender_jammer_notice_cb(void* context, uint32_t index) {
    DefenderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

// Map RSSI (dBm) to a 0..JAMMER_BINS energy level for the waterfall bar.
static uint8_t defender_jammer_level(float rssi) {
    float span = (float)(JAM_RSSI_CEIL - JAM_RSSI_FLOOR);
    float f = (rssi - (float)JAM_RSSI_FLOOR) / span;
    if(f < 0.0f) f = 0.0f;
    if(f > 1.0f) f = 1.0f;
    return (uint8_t)(f * JAMMER_BINS);
}

// Timer thread: sample RSSI, feed the detector, scroll the waterfall.
static void defender_jammer_timer_cb(void* context) {
    UNUSED(context);
    if(!s_running) return;

    float raw = subghz_devices_get_rssi(s_device);
    float rssi = subghz_threshold_get_rssi_data(s_rssi, raw).rssi;
    uint32_t now = furi_get_tick();

    bool was = s_jam.jamming;
    defender_jammer_feed(&s_jam, rssi, false, now); // RSSI-only (packet_decoded=false)

    uint8_t level = defender_jammer_level(rssi);
    uint8_t col[JAMMER_BINS];
    for(int i = 0; i < JAMMER_BINS; i++) col[i] = (i < level) ? 255 : 0;
    defender_waterfall_view_push(
        s_app->waterfall, col, JAMMER_BINS, (int8_t)(level ? level - 1 : 0));

    s_jam_rssi = (int8_t)rssi;
    if(!was && s_jam.jamming) {
        float over = rssi - s_jam.floor_ema;
        s_jam_over_db = (int8_t)over;
        s_jam_edge = true; // consumed by the dispatcher tick
    }
}

// Show the "No Sub-GHz" notice using the shared submenu (no extra view module).
// `reason` names the exact detection step that failed (diagnostic).
static void defender_jammer_show_no_radio(DefenderApp* app, const char* reason) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, reason ? reason : "No Sub-GHz radio");
    submenu_add_item(app->submenu, "Back", JAMMER_NO_RADIO, defender_jammer_notice_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewSubmenu);
}

void defender_scene_jammer_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    s_app = app;
    s_running = false;
    s_began = false;
    s_inited = false;
    s_jam_edge = false;
    memset(&s_jam, 0, sizeof(s_jam));

    defender_waterfall_view_set_label(app->waterfall, "433.92 MHz");
    view_set_input_callback(app->waterfall, defender_jammer_ok_input);

    subghz_devices_init();
    s_inited = true;
    s_device = subghz_devices_get_by_name(JAMMER_DEVICE);
    if(!s_device) {
        defender_jammer_show_no_radio(app, "no cc1101_ext dev");
        return;
    }

    // Power the external CC1101 from the 5V OTG boost before probing it (same as
    // the main Sub-GHz app). Without this the SPI probe below fails.
    uint8_t otg_tries = 0;
    while(!furi_hal_power_is_otg_enabled() && otg_tries++ < 5) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }

    s_began = subghz_devices_begin(s_device);
    if(!s_began) {
        defender_jammer_show_no_radio(app, "begin() failed");
        return;
    }
    subghz_devices_reset(s_device);
    if(!subghz_devices_is_connect(s_device)) {
        defender_jammer_show_no_radio(app, "not connected (SPI)");
        return;
    }
    if(!subghz_devices_is_frequency_valid(s_device, JAMMER_FREQ)) {
        defender_jammer_show_no_radio(app, "freq invalid");
        return;
    }

    subghz_devices_load_preset(s_device, FuriHalSubGhzPreset2FSKDev238Async, NULL);
    subghz_devices_set_frequency(s_device, JAMMER_FREQ);
    subghz_devices_set_rx(s_device);
    s_rssi = subghz_threshold_rssi_alloc();

    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewWaterfall);

    s_running = true;
    s_timer = furi_timer_alloc(defender_jammer_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(s_timer, 50);
}

bool defender_scene_jammer_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    DefenderApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == JAMMER_NO_RADIO) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        } else if(event.event == JAMMER_OK_LOCATE) {
            snprintf(app->locator_label, sizeof(app->locator_label), "Jammer 433.92");
            app->locator_floor = JAM_RSSI_FLOOR;
            app->locator_ceil = JAM_RSSI_CEIL;
            app->locator_active = true;
            scene_manager_next_scene(app->scene_manager, DefenderSceneLocator);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // Raise the alert on the dispatcher thread (single-threaded alert bus).
        if(s_jam_edge) {
            s_jam_edge = false;
            DefenderAlert a;
            memset(&a, 0, sizeof(a));
            a.cat = DefenderCatSubghzJam;
            a.severity = 2;
            a.rssi = s_jam_rssi;
            a.ts_ms = furi_get_tick();
            snprintf(a.msg, sizeof(a.msg), "RF jam 433.92 +%ddB", (int)s_jam_over_db);
            defender_alert_raise(&app->alert, &a);
        }
        consumed = true;
    }

    return consumed;
}

void defender_scene_jammer_on_exit(void* context) {
    furi_assert(context);
    UNUSED(context);

    s_running = false;
    if(s_timer) {
        furi_timer_stop(s_timer);
        furi_timer_free(s_timer);
        s_timer = NULL;
    }

    if(s_began) {
        subghz_devices_idle(s_device);
        subghz_devices_sleep(s_device);
        subghz_devices_end(s_device);
        s_began = false;
    }
    if(s_rssi) {
        subghz_threshold_rssi_free(s_rssi);
        s_rssi = NULL;
    }
    if(s_inited) {
        subghz_devices_deinit();
        s_inited = false;
    }
    s_device = NULL;
    // Drop the 5V OTG boost we enabled for the external CC1101.
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
    furi_hal_light_blink_stop();  // Belt-and-suspenders: clear any lingering blink on scene exit.
}
