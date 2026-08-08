#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"
#ifndef PUBLIC_BUILD
#include "../helpers/subbrute_resume.h" // resumable brute sessions (PRIVATE-only)
#endif

#define TAG "SubGhzSceneBfRunAttack"

#ifndef PUBLIC_BUILD
#define SUBBRUTE_RESUME_CHECKPOINT_MS 10000u // save a resume point at most this often

static uint32_t s_resume_last_tick; // throttles checkpoint flash writes (UI thread)

// Snapshot the live sweep and persist it. Called only from the UI/scene thread
// (Tick / on_exit), never the worker TX loop. Non-resumable attacks
// (DeBruijn / LoadFile) are skipped by subbrute_resume_is_valid.
static void subghz_scene_bf_run_attack_checkpoint(SubGhz* subghz) {
    SubBruteDevice* d = subghz->subbrute_device;
    if(!d) return;
    SubBruteResume r = {
        .schema_version = SUBBRUTE_RESUME_SCHEMA,
        .attack = d->attack,
        .current_step = d->current_step,
        .extra_repeats = d->extra_repeats,
        .two_bytes = d->two_bytes ? 1u : 0u,
        .opencode = d->opencode,
    };
    if(subbrute_resume_is_valid(&r)) subbrute_resume_save(&r);
}
#endif

static void subghz_scene_bf_run_attack_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

static void
    subghz_scene_bf_run_attack_device_state_changed(void* context, SubBruteWorkerState state) {
    furi_assert(context);

    SubGhz* subghz = (SubGhz*)context;

    if(state == SubBruteWorkerStateIDLE) {
        // Can't be IDLE on this step!
        view_dispatcher_send_custom_event(subghz->view_dispatcher, SubBruteCustomEventTypeError);
    } else if(state == SubBruteWorkerStateFinished) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubBruteCustomEventTypeTransmitFinished);
    }
}

void subghz_scene_bf_run_attack_on_exit(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;
    notification_message(subghz->notifications, &sequence_blink_stop);
    subbrute_worker_stop(subghz->subbrute_worker);
#ifndef PUBLIC_BUILD
    // Persist a final checkpoint if the sweep is unfinished (a completed sweep was
    // already cleared on TransmitFinished). current_step+1 < max_value == work left.
    SubBruteDevice* d = subghz->subbrute_device;
    if(d && d->max_value > 0 && (d->current_step + 1) < d->max_value) {
        subghz_scene_bf_run_attack_checkpoint(subghz);
    }
#endif
}

void subghz_scene_bf_run_attack_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;

    subbrute_attack_view_set_callback(view, subghz_scene_bf_run_attack_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdBfAttack);

    subbrute_worker_set_callback(
        subghz->subbrute_worker, subghz_scene_bf_run_attack_device_state_changed, subghz);

    if(!subbrute_worker_is_running(subghz->subbrute_worker)) {
        subbrute_worker_set_step(
            subghz->subbrute_worker, subghz->subbrute_device->current_step);
        if(!subbrute_worker_start(subghz->subbrute_worker)) {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubBruteCustomEventTypeError);
        } else {
            notification_message(subghz->notifications, &sequence_single_vibro);
            notification_message(subghz->notifications, &sequence_blink_start_yellow);
        }
    }
#ifndef PUBLIC_BUILD
    s_resume_last_tick = furi_get_tick(); // arm the checkpoint throttle
#endif
}

bool subghz_scene_bf_run_attack_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = (SubGhz*)context;
    SubBruteAttackView* view = subghz->subbrute_attack_view;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint64_t step = subbrute_worker_get_step(subghz->subbrute_worker);
        subghz->subbrute_device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);

        if(event.event == SubBruteCustomEventTypeTransmitFinished) {
            notification_message(subghz->notifications, &sequence_display_backlight_on);
            notification_message(subghz->notifications, &sequence_double_vibro);
#ifndef PUBLIC_BUILD
            subbrute_resume_clear(); // sweep complete — nothing left to resume
#endif
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(
            event.event == SubBruteCustomEventTypeTransmitNotStarted ||
            event.event == SubBruteCustomEventTypeBackPressed) {
            if(subbrute_worker_is_running(subghz->subbrute_worker)) {
                // Notify
                notification_message(subghz->notifications, &sequence_single_vibro);
            }
            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(subghz->notifications, &sequence_error);

            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                subghz->scene_manager, SubGhzSceneBfSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeUpdateView) {
            //subbrute_attack_view_set_current_step(view, subghz->subbrute_device->current_step);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint64_t step = subbrute_worker_get_step(subghz->subbrute_worker);
        subghz->subbrute_device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
#ifndef PUBLIC_BUILD
        // Throttled resume checkpoint (UI thread; never the worker TX loop).
        uint32_t now = furi_get_tick();
        if(now - s_resume_last_tick >= furi_ms_to_ticks(SUBBRUTE_RESUME_CHECKPOINT_MS)) {
            s_resume_last_tick = now;
            subghz_scene_bf_run_attack_checkpoint(subghz);
        }
#endif
        consumed = true;
    }

    return consumed;
}
