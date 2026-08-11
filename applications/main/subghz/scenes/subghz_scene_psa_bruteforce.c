#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"

#include <furi.h>

#ifndef PUBLIC_BUILD
#include <lib/subghz/protocols/psa_bf_types.h>
#include <lib/subghz/protocols/psa_bf_core.h>
#include <lib/subghz/protocols/psa_crypto_bf.h>
#include <string.h>

#define TAG            "SubGhzScenePsaBf"
#define PSA_BF_TICK_MS 250

/* PSA (Peugeot/Citroen/...) rolling-code brute force. Ported from the removed
 * ProtoPirate app into the native Sub-GHz app — the BF engine itself lives in
 * lib/subghz/protocols/ (psa_crypto_bf / psa_bf_core); this scene is just the
 * host: it loads the captured signal's Key/Key_2 into a PsaBfState, runs the
 * search on a worker thread, shows progress, and reports the recovered key.
 * Reached from the receiver-info screen's "Brute" button on a PSA capture. */

/* Scene-local state (single instance at a time, like the other worker scenes). */
static PsaBfState* s_state = NULL;
static FuriThread* s_thread = NULL;
static FuriTimer* s_timer = NULL;

static void subghz_scene_psa_bf_timer_cb(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventScenePsaBfTick);
}

static void subghz_scene_psa_bf_render(SubGhz* subghz) {
    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "PSA Brute Force");

    char line[96];
    uint8_t st = s_state ? s_state->status : PSA_BF_STATUS_NOT_FOUND;

    if(st == PSA_BF_STATUS_RUNNING || st == PSA_BF_STATUS_IDLE) {
        uint32_t cur = s_state->progress_current;
        uint32_t tot = s_state->progress_total;
        uint32_t pct = tot ? (uint32_t)(((uint64_t)cur * 100) / tot) : 0;
        snprintf(
            line, sizeof(line), "Searching key...\n%lu%%   %lu / %lu", (unsigned long)pct,
            (unsigned long)cur, (unsigned long)tot);
        widget_add_string_multiline_element(
            subghz->widget, 0, 16, AlignLeft, AlignTop, FontSecondary, line);
        widget_add_string_element(
            subghz->widget, 64, 63, AlignCenter, AlignBottom, FontSecondary, "Back = cancel");
    } else if(st == PSA_BF_STATUS_FOUND) {
        snprintf(
            line, sizeof(line), "FOUND\nSN:0x%08lX  Btn:%u\nCnt:0x%04lX  Seed:0x%08lX\nKey:%08lX%08lX",
            (unsigned long)s_state->decrypted_serial, (unsigned)s_state->decrypted_button,
            (unsigned long)s_state->decrypted_counter, (unsigned long)s_state->decrypted_seed,
            (unsigned long)s_state->key1_high, (unsigned long)s_state->key1_low);
        widget_add_string_multiline_element(
            subghz->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, line);
        widget_add_string_element(
            subghz->widget, 64, 63, AlignCenter, AlignBottom, FontSecondary, "Back");
    } else if(st == PSA_BF_STATUS_NOT_FOUND) {
        widget_add_string_element(
            subghz->widget, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Key not found");
    } else { /* PSA_BF_STATUS_CANCELLED */
        widget_add_string_element(
            subghz->widget, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Cancelled");
    }
}

void subghz_scene_psa_bruteforce_on_enter(void* context) {
    SubGhz* subghz = context;

    s_state = malloc(sizeof(PsaBfState));
    memset(s_state, 0, sizeof(PsaBfState));
    s_state->status = PSA_BF_STATUS_IDLE;

    /* Load Key + Key_2 from the selected capture (the PSA decoder serialised
     * them). Without those fields there is nothing to brute-force. */
    FlipperFormat* fff = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    bool ok = (fff != NULL) && psa_bf_state_from_flipper_format(s_state, fff);

    if(!ok) {
        widget_reset(subghz->widget);
        widget_add_string_element(
            subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "PSA Brute Force");
        widget_add_string_multiline_element(
            subghz->widget, 0, 22, AlignLeft, AlignTop, FontSecondary,
            "No PSA key data in\nthis capture.");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    s_state->status = PSA_BF_STATUS_RUNNING;
    subghz_scene_psa_bf_render(subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);

    s_thread = furi_thread_alloc_ex("PsaBf", 4096, psa_brute_force_thread_entry, s_state);
    /* Below Normal so the UI/timer stay responsive while the search runs. */
    furi_thread_set_priority(s_thread, FuriThreadPriorityLow);
    furi_thread_start(s_thread);

    s_timer = furi_timer_alloc(subghz_scene_psa_bf_timer_cb, FuriTimerTypePeriodic, subghz);
    furi_timer_start(s_timer, furi_kernel_get_tick_frequency() * PSA_BF_TICK_MS / 1000);
}

bool subghz_scene_psa_bruteforce_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventScenePsaBfTick) {
        subghz_scene_psa_bf_render(subghz);
        return true;
    }
    if(event.type == SceneManagerEventTypeBack) {
        if(s_state) s_state->cancel = 1; /* signal the worker to stop; on_exit joins */
        return false; /* let the scene manager pop back to receiver-info */
    }
    return false;
}

void subghz_scene_psa_bruteforce_on_exit(void* context) {
    SubGhz* subghz = context;

    if(s_timer) {
        furi_timer_stop(s_timer);
        furi_timer_free(s_timer);
        s_timer = NULL;
    }
    if(s_state) s_state->cancel = 1;
    if(s_thread) {
        furi_thread_join(s_thread);
        furi_thread_free(s_thread);
        s_thread = NULL;
    }
    if(s_state) {
        free(s_state);
        s_state = NULL;
    }
    widget_reset(subghz->widget);
}

#else
// PUBLIC_BUILD: PSA (Peugeot/Citroen) rolling-code brute force is a car feature,
// stripped from public builds (guardrail — no car material on public). Inert stubs
// keep the scene registry/enum valid so navigation still compiles; the scene is
// effectively unreachable (PSA captures can't occur without the PSA decoder).
void subghz_scene_psa_bruteforce_on_enter(void* context) {
    UNUSED(context);
}
bool subghz_scene_psa_bruteforce_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}
void subghz_scene_psa_bruteforce_on_exit(void* context) {
    UNUSED(context);
}
#endif
