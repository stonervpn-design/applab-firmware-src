#include "../subghz_i.h"

// PRIVATE-only, same reasoning as subghz_scene_protocol_fuzzer_config.c: the
// FuzzerWorker/SubGhzViewFuzzer types + fuzzer_* struct fields this file uses
// only exist under #ifndef PUBLIC_BUILD in subghz_i.h, and nothing in a
// PUBLIC_BUILD compile ever references these functions (ADD_SCENE is guarded
// too), so this file must compile to an empty translation unit there.
#ifndef PUBLIC_BUILD

#include <storage/storage.h>

#define TAG "SubGhzSceneProtocolFuzzerRun"

// Run scene for the Protocol Fuzzer (PRIVATE-only). Owns the FuzzerWorker +
// SubGhzViewFuzzer for the duration of the scene (lazy-alloc on_enter,
// stop+free on_exit — mirrors subghz_scene_jammer.c / subghz_scene_spectrum.c).
// The view's SubGhzViewFuzzerEvent (StartPause/Step/Mark/Stop) is bridged to a
// custom event exactly like subghz_scene_jammer_callback/spectrum's view_cb;
// the worker's per-frame callback runs on the worker thread and pushes
// straight into the (model-locked) view, same as the spectrum row callback.

static bool s_fuzzer_paused = false;

// Cache of the last data pushed to the view, so Stop/Mark can re-push a
// tweaked copy (e.g. tx_on cleared, a "[Marked]" suffix) without needing a
// separate "peek current state" API on either the worker or the view.
static char s_last_mode_str[48] = {0};
static char s_last_mutation_str[48] = {0};
static uint32_t s_last_index = 0;
static uint32_t s_last_total = 0;

static const char* fuzzer_run_strategy_name(FuzzStrategy strategy) {
    switch(strategy) {
    case FuzzStrategyBitFlipSweep:
        return "BitFlip";
    case FuzzStrategyBoundary:
        return "Boundary";
    case FuzzStrategyLength:
        return "Length";
    case FuzzStrategyRandom:
        return "Random";
    case FuzzStrategyFieldWalk:
        return "FieldWalk";
    default:
        return "?";
    }
}

static void fuzzer_run_mutation_desc(FuzzStrategy strategy, uint32_t index, char* out, size_t cap) {
    switch(strategy) {
    case FuzzStrategyBitFlipSweep:
        snprintf(out, cap, "bit %lu flip", (unsigned long)index);
        break;
    case FuzzStrategyBoundary:
        snprintf(out, cap, "boundary case %lu", (unsigned long)index);
        break;
    case FuzzStrategyLength:
        snprintf(out, cap, "length delta #%lu", (unsigned long)index);
        break;
    case FuzzStrategyRandom:
        snprintf(out, cap, "random iter %lu", (unsigned long)index);
        break;
    case FuzzStrategyFieldWalk:
        snprintf(out, cap, "field walk %lu", (unsigned long)index);
        break;
    default:
        snprintf(out, cap, "frame %lu", (unsigned long)index);
        break;
    }
}

// Runs on the FuzzerWorker thread (auto-sweep) or inline on the GUI thread
// (fuzzer_worker_step()). subghz_view_fuzzer_set_data() is model-locked, so
// this is cross-thread safe; on_exit stops+joins the worker before freeing
// the view, so no callback can ever race a teardown.
static void fuzzer_run_worker_callback(void* context, uint32_t index, uint32_t total) {
    SubGhz* subghz = context;
    if(!subghz->fuzzer_view) return;

    snprintf(
        s_last_mode_str,
        sizeof(s_last_mode_str),
        "%s: %s / %s",
        subghz->fuzzer_cfg.mode == FuzzModeProtocol ? "Protocol" : "RAW",
        subghz->fuzzer_cfg.mode == FuzzModeProtocol ? subghz->fuzzer_protocol : "-",
        fuzzer_run_strategy_name(subghz->fuzzer_cfg.strategy));
    fuzzer_run_mutation_desc(
        subghz->fuzzer_cfg.strategy, index, s_last_mutation_str, sizeof(s_last_mutation_str));
    s_last_index = index;
    s_last_total = total;

    subghz_view_fuzzer_set_data(
        subghz->fuzzer_view,
        subghz->fuzzer_freq,
        s_last_mode_str,
        s_last_mutation_str,
        index,
        total,
        true);
}

static void fuzzer_run_view_callback(SubGhzViewFuzzerEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_protocol_fuzzer_run_on_enter(void* context) {
    SubGhz* subghz = context;

    s_fuzzer_paused = false;
    s_last_mode_str[0] = '\0';
    s_last_mutation_str[0] = '\0';
    s_last_index = 0;
    s_last_total = 0;

    // Lazy-load: allocate + register the Fuzzer view only while this scene is
    // active (no-PSRAM heap discipline, mirrors Jammer/Spectrum). Idempotent.
    if(!subghz->fuzzer_view) {
        subghz->fuzzer_view = subghz_view_fuzzer_alloc();
        view_dispatcher_add_view(
            subghz->view_dispatcher,
            SubGhzViewIdFuzzer,
            subghz_view_fuzzer_get_view(subghz->fuzzer_view));
    }
    subghz_view_fuzzer_set_callback(subghz->fuzzer_view, fuzzer_run_view_callback, subghz);

    if(!subghz->fuzzer_worker) {
        subghz->fuzzer_worker = fuzzer_worker_alloc();
    }
    fuzzer_worker_set_config(
        subghz->fuzzer_worker,
        &subghz->fuzzer_cfg,
        subghz->fuzzer_protocol,
        subghz->fuzzer_freq,
        subghz->fuzzer_preset,
        subghz->fuzzer_te,
        subghz->fuzzer_repeat);
    fuzzer_worker_set_callback(subghz->fuzzer_worker, fuzzer_run_worker_callback, subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdFuzzer);
}

// Ensures the worker is paused (not stopped) before a mark, per
// fuzzer_worker_mark_last()'s doc: it refuses while auto-running-and-unpaused.
static void fuzzer_run_do_mark(SubGhz* subghz) {
    if(fuzzer_worker_running(subghz->fuzzer_worker) && !s_fuzzer_paused) {
        fuzzer_worker_pause(subghz->fuzzer_worker, true);
        s_fuzzer_paused = true;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("subghz"));
    storage_simply_mkdir(storage, EXT_PATH("subghz/fuzzer"));
    furi_record_close(RECORD_STORAGE);

    uint32_t idx = fuzzer_worker_index(subghz->fuzzer_worker);
    uint32_t last_idx = (idx > 0) ? (idx - 1) : 0; // index of the frame just sent

    char path[64];
    snprintf(path, sizeof(path), "%s/mark_%05lu.sub", EXT_PATH("subghz/fuzzer"), (unsigned long)last_idx);

    bool ok = fuzzer_worker_mark_last(subghz->fuzzer_worker, path);
    FURI_LOG_I(TAG, "mark_last(%s) -> %s", path, ok ? "OK" : "FAILED");

    if(subghz->fuzzer_view) {
        // Sized for the worst case: sizeof(s_last_mutation_str)-1 + " " +
        // strlen("[Mark FAILED]") + NUL, so -Werror=format-truncation can
        // prove this never truncates regardless of s_last_mutation_str's
        // runtime content.
        char mutation_buf[sizeof(s_last_mutation_str) + 16];
        snprintf(
            mutation_buf,
            sizeof(mutation_buf),
            "%s %s",
            s_last_mutation_str,
            ok ? "[Marked]" : "[Mark FAILED]");
        subghz_view_fuzzer_set_data(
            subghz->fuzzer_view,
            subghz->fuzzer_freq,
            s_last_mode_str,
            mutation_buf,
            s_last_index,
            s_last_total,
            fuzzer_worker_running(subghz->fuzzer_worker) && !s_fuzzer_paused);
    }
}

bool subghz_scene_protocol_fuzzer_run_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(!subghz->fuzzer_worker) return false;

    switch(event.event) {
    case SubGhzViewFuzzerEventStartPause:
        if(!fuzzer_worker_running(subghz->fuzzer_worker)) {
            if(fuzzer_worker_start(subghz->fuzzer_worker)) {
                s_fuzzer_paused = false;
            } else {
                FURI_LOG_W(TAG, "fuzzer_worker_start failed (unconfigured or bad frequency)");
            }
        } else {
            s_fuzzer_paused = !s_fuzzer_paused;
            fuzzer_worker_pause(subghz->fuzzer_worker, s_fuzzer_paused);
            if(subghz->fuzzer_view) {
                subghz_view_fuzzer_set_data(
                    subghz->fuzzer_view,
                    subghz->fuzzer_freq,
                    s_last_mode_str,
                    s_last_mutation_str,
                    s_last_index,
                    s_last_total,
                    !s_fuzzer_paused);
            }
        }
        return true;

    case SubGhzViewFuzzerEventStep:
        fuzzer_worker_step(subghz->fuzzer_worker); // no-op / logged internally if disallowed
        return true;

    case SubGhzViewFuzzerEventMark:
        fuzzer_run_do_mark(subghz);
        return true;

    case SubGhzViewFuzzerEventStop:
        fuzzer_worker_stop(subghz->fuzzer_worker);
        s_fuzzer_paused = false;
        if(subghz->fuzzer_view) {
            subghz_view_fuzzer_set_data(
                subghz->fuzzer_view,
                subghz->fuzzer_freq,
                s_last_mode_str,
                s_last_mutation_str,
                s_last_index,
                s_last_total,
                false);
        }
        return true;

    default:
        return false;
    }
}

void subghz_scene_protocol_fuzzer_run_on_exit(void* context) {
    SubGhz* subghz = context;

    // Stop + join the worker FIRST so no per-frame callback can touch the
    // view we're about to free (mirrors subghz_scene_spectrum_on_exit).
    if(subghz->fuzzer_worker) {
        fuzzer_worker_stop(subghz->fuzzer_worker);
        fuzzer_worker_free(subghz->fuzzer_worker);
        subghz->fuzzer_worker = NULL;
    }

    if(subghz->fuzzer_view) {
        view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdFuzzer);
        subghz_view_fuzzer_free(subghz->fuzzer_view);
        subghz->fuzzer_view = NULL;
    }
}

#endif // PUBLIC_BUILD
