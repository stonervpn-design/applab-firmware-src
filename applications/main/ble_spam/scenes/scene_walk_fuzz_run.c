#include "../ble_spam_app.h"

// PRIVATE-only, same reasoning as scene_walk_target_info.c / scene_walk_fuzz_config.c.
#ifndef PUBLIC_BUILD

#include "../ble_walk_hal.h"
#include "../ble_walk_fuzz.h"
#include "../views/ble_fuzz_view.h"

#include <storage/storage.h>
#include <stdio.h>

#define TAG "BleWalkFuzzRun"

// Run scene for the BLE GATT Fuzzer/DoS (PRIVATE-only). Owns the BleWalkFuzz
// engine (Task 4) + BleFuzzView (Task 5) for the duration of the scene
// (lazy-alloc on_enter, stop+free on_exit -- mirrors
// subghz_scene_protocol_fuzzer_run.c). The view's BleFuzzViewEvent
// (StartPause/Step/Mark/Stop) is bridged to a custom event exactly like
// subghz's fuzzer_run_view_callback; the engine's per-iteration callback runs
// on the engine thread and pushes straight into the (model-locked) view.

static bool s_fuzz_paused = false;

static const char* fuzz_run_mode_name(BleFuzzMode mode) {
    switch(mode) {
    case BleFuzzModeSweep:
        return "Sweep";
    case BleFuzzModeStack:
        return "Stack";
    case BleFuzzModeDos:
        return "DoS";
    default:
        return "?";
    }
}

// Pushes the engine's current state into the view. Safe to call from either
// the engine thread (per-iteration callback) or the GUI thread (on_enter,
// on_event) -- ble_fuzz_view_set_data() is model-locked.
static void fuzz_run_push_view(BleSpamApp* app) {
    if(!app->fuzz_view || !app->walk_fuzz) return;

    char addr_str[20];
    const uint8_t* a = app->fuzz_target.addr;
    snprintf(
        addr_str,
        sizeof(addr_str),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        a[0],
        a[1],
        a[2],
        a[3],
        a[4],
        a[5]);

    ble_fuzz_view_set_data(
        app->fuzz_view,
        app->fuzz_target.name[0] ? app->fuzz_target.name : NULL,
        addr_str,
        fuzz_run_mode_name(app->fuzz_mode),
        ble_walk_fuzz_index(app->walk_fuzz),
        ble_walk_fuzz_total(app->walk_fuzz),
        ble_walk_fuzz_counter(app->walk_fuzz),
        ble_walk_fuzz_running(app->walk_fuzz) && !s_fuzz_paused);
}

static void fuzz_run_engine_callback(void* context, uint32_t index, uint32_t counter) {
    UNUSED(index);
    UNUSED(counter);
    BleSpamApp* app = context;
    fuzz_run_push_view(app);
}

// Translates the view's BleFuzzViewEvent into this app's BleSpamWalkEvent
// numbering space before dispatching -- same reasoning as
// subghz_scene_protocol_fuzzer_run.c's fuzzer_run_view_callback: keeps these
// as real BleSpamWalkEvent members (rather than sending the raw
// BleFuzzViewEvent 0-3 values) so they don't collide with
// BleSpamCustomEventWalkConnect/WalkTargetInfoArmDos in this enum's space.
static void fuzz_run_view_callback(BleFuzzViewEvent event, void* context) {
    furi_assert(context);
    BleSpamApp* app = context;
    BleSpamWalkEvent custom_event;
    switch(event) {
    case BleFuzzViewEventStartPause:
        custom_event = BleSpamCustomEventWalkFuzzStartPause;
        break;
    case BleFuzzViewEventStep:
        custom_event = BleSpamCustomEventWalkFuzzStep;
        break;
    case BleFuzzViewEventMark:
        custom_event = BleSpamCustomEventWalkFuzzMark;
        break;
    case BleFuzzViewEventStop:
    default:
        custom_event = BleSpamCustomEventWalkFuzzStop;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, custom_event);
}

void ble_spam_scene_walk_fuzz_run_on_enter(void* context) {
    BleSpamApp* app = context;

    s_fuzz_paused = false;

    // Lazy-alloc: the engine only exists while this scene is active (no-PSRAM
    // heap discipline, mirrors the Sub-GHz fuzzer's worker + every other
    // ble_spam scene that owns a worker). Idempotent -- app_free's safety net
    // may have already freed a stale one, but on_exit always nulls the
    // pointer, so this is never a double-alloc.
    if(!app->walk_fuzz) {
        app->walk_fuzz = ble_walk_fuzz_alloc();
    }

    ble_walk_fuzz_set_target(app->walk_fuzz, &app->fuzz_target);
    ble_walk_fuzz_config(
        app->walk_fuzz,
        app->fuzz_mode,
        app->fuzz_cfg,
        app->fuzz_char_handle,
        app->fuzz_write_norsp,
        app->fuzz_rate_ms,
        app->fuzz_count);
    ble_walk_fuzz_set_callback(app->walk_fuzz, fuzz_run_engine_callback, app);

    ble_fuzz_view_set_callback(app->fuzz_view, fuzz_run_view_callback, app);
    fuzz_run_push_view(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewFuzz);
}

// Ensures the engine is paused (not stopped) before a mark, per
// ble_walk_fuzz_mark_last()'s doc: it refuses while running-and-unpaused.
static void fuzz_run_do_mark(BleSpamApp* app) {
    if(ble_walk_fuzz_running(app->walk_fuzz) && !s_fuzz_paused) {
        ble_walk_fuzz_pause(app->walk_fuzz, true);
        s_fuzz_paused = true;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("ble_fuzz"));
    furi_record_close(RECORD_STORAGE);

    uint32_t idx = ble_walk_fuzz_index(app->walk_fuzz);
    uint32_t last_idx = (idx > 0) ? (idx - 1) : 0; // index of the iteration just run

    char path[64];
    snprintf(
        path, sizeof(path), "%s/mark_%05lu.txt", EXT_PATH("ble_fuzz"), (unsigned long)last_idx);

    bool ok = ble_walk_fuzz_mark_last(app->walk_fuzz, path);
    FURI_LOG_I(TAG, "mark_last(%s) -> %s", path, ok ? "OK" : "FAILED");

    fuzz_run_push_view(app);
}

bool ble_spam_scene_walk_fuzz_run_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(!app->walk_fuzz) return false;

    switch(event.event) {
    case BleSpamCustomEventWalkFuzzStartPause:
        if(!ble_walk_fuzz_running(app->walk_fuzz)) {
            if(ble_walk_fuzz_start(app->walk_fuzz)) {
                s_fuzz_paused = false;
            } else {
                FURI_LOG_W(TAG, "ble_walk_fuzz_start failed (no target/connection/handle)");
            }
        } else {
            s_fuzz_paused = !s_fuzz_paused;
            ble_walk_fuzz_pause(app->walk_fuzz, s_fuzz_paused);
        }
        fuzz_run_push_view(app);
        return true;

    case BleSpamCustomEventWalkFuzzStep:
        ble_walk_fuzz_step(app->walk_fuzz); // no-op / logged internally if disallowed
        fuzz_run_push_view(app);
        return true;

    case BleSpamCustomEventWalkFuzzMark:
        fuzz_run_do_mark(app);
        return true;

    case BleSpamCustomEventWalkFuzzStop:
        ble_walk_fuzz_stop(app->walk_fuzz);
        s_fuzz_paused = false;
        fuzz_run_push_view(app);
        return true;

    default:
        return false;
    }
}

void ble_spam_scene_walk_fuzz_run_on_exit(void* context) {
    BleSpamApp* app = context;

    // Stop + join the engine FIRST so no per-iteration callback can touch the
    // view we're about to leave (mirrors subghz_scene_protocol_fuzzer_run's
    // on_exit ordering).
    if(app->walk_fuzz) {
        ble_walk_fuzz_stop(app->walk_fuzz);
        ble_walk_fuzz_free(app->walk_fuzz);
        app->walk_fuzz = NULL;
    }
}

#endif // PUBLIC_BUILD
