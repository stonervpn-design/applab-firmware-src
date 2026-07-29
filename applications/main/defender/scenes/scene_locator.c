#include "../defender_app_i.h"
#include "../defender_locator_map.h"

#include <furi.h>
#include <furi_hal_speaker.h>

/**
 * Locator Test scene.
 *
 * Drives the locator view with a synthetic triangle-wave RSSI so the bar
 * graph and proximity beep can be verified on-device with no real radio.
 * The periodic timer advances the wave, pushes it into the locator, and
 * beeps at a cadence derived from proximity (fast when "hot", slow when
 * "cold"). A single app instance means file-scope state is safe here.
 */

#define LOC_TEST_FLOOR (-90)
#define LOC_TEST_CEIL  (-40)
#define LOC_TEST_STEP  3
#define LOC_BEEP_MS    40

static FuriTimer* s_loc_timer = NULL;
static int8_t s_loc_rssi = LOC_TEST_FLOOR;
static int8_t s_loc_dir = LOC_TEST_STEP;

// Runs in the FuriTimer thread: advance triangle wave, push, beep, re-arm.
static void defender_scene_locator_timer_cb(void* context) {
    DefenderApp* app = context;

    int16_t next = (int16_t)s_loc_rssi + s_loc_dir;
    if(next >= LOC_TEST_CEIL) {
        next = LOC_TEST_CEIL;
        s_loc_dir = -LOC_TEST_STEP;
    } else if(next <= LOC_TEST_FLOOR) {
        next = LOC_TEST_FLOOR;
        s_loc_dir = LOC_TEST_STEP;
    }
    s_loc_rssi = (int8_t)next;

    defender_locator_push(app->locator, s_loc_rssi, false);

    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_start(2000.0f, 0.5f);
        furi_delay_ms(LOC_BEEP_MS);
        furi_hal_speaker_stop();
    }

    uint32_t period =
        defender_locator_beep_period_ms(s_loc_rssi, false, LOC_TEST_FLOOR, LOC_TEST_CEIL);
    if(period == 0) period = 1200;
    furi_timer_restart(s_loc_timer, period);
}

void defender_scene_locator_on_enter(void* context) {
    furi_assert(context);
    DefenderApp* app = context;

    // A detector scene may hand off an offender (label + bounds) before pushing
    // this scene. Radios are stopped on scene transition, so the bar is driven
    // by the synthetic sweep in either case (live cross-radio tracking = Phase 2).
    const char* label = "Locator Test";
    int8_t floor = LOC_TEST_FLOOR;
    int8_t ceil = LOC_TEST_CEIL;
    if(app->locator_active) {
        label = app->locator_label;
        floor = app->locator_floor;
        ceil = app->locator_ceil;
    }

    defender_locator_set_label(app->locator, label);
    defender_locator_set_bounds(app->locator, floor, ceil);

    s_loc_rssi = floor;
    s_loc_dir = LOC_TEST_STEP;
    defender_locator_push(app->locator, s_loc_rssi, false);

    view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewLocator);

    if(!furi_hal_speaker_acquire(1000)) {
        // Speaker busy: bar still animates, just no beep.
    }

    s_loc_timer =
        furi_timer_alloc(defender_scene_locator_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(s_loc_timer, 300);
}

bool defender_scene_locator_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void defender_scene_locator_on_exit(void* context) {
    furi_assert(context);
    DefenderApp* app = context;
    app->locator_active = false; // consume the handoff

    if(s_loc_timer) {
        furi_timer_stop(s_loc_timer);
        furi_timer_free(s_loc_timer);
        s_loc_timer = NULL;
    }

    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
}
