#include "../ble_spam_app.h"
#include "../ble_tracker_hal.h"
#include "../views/tracker_geiger_view.h"

#include <esp_log.h>
#include <furi_hal_speaker.h>
#include <string.h>

#define TAG "BleTrackerGeiger"
#define STALE_TIMEOUT_MS 4000
#define BEEP_DURATION_MS 50

static int8_t s_prev_smooth;      // previous smoothed value (for trend)
static bool s_have_prev;
static uint32_t s_peak_decay_ms;  // last time the peak was decayed

static uint32_t period_for_rssi(int8_t rssi) {
    if(rssi > -45) return 80;
    if(rssi > -60) return 160;
    if(rssi > -72) return 300;
    if(rssi > -84) return 600;
    return 1200;
}

static float pitch_for_rssi(int8_t rssi) {
    int32_t hz = 600 + ((int32_t)rssi + 100) * 30;
    if(hz < 600) hz = 600;
    if(hz > 3500) hz = 3500;
    return (float)hz;
}

// Runs in FuriTimer thread. Reads cached state from app, emits beep.
static void tracker_geiger_timer_callback(void* context) {
    BleSpamApp* app = context;

    int8_t rssi = app->tracker_current_rssi;
    bool stale = app->tracker_current_stale;

    uint32_t period;
    if(stale) {
        period = 1200;
    } else {
        period = period_for_rssi(rssi);
        // Acquire on THIS (timer) thread: the speaker mutex is owner-per-thread, so
        // acquiring in on_enter (scene thread) left is_mine() false here and the beep
        // never played. The beep is self-contained, so acquire+release per tick.
        if(furi_hal_speaker_acquire(20)) {
            furi_hal_speaker_start(pitch_for_rssi(rssi), 0.5f);
            furi_delay_ms(BEEP_DURATION_MS);
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
    }

    if(period != app->tracker_current_period) {
        app->tracker_current_period = period;
        furi_timer_restart(app->tracker_geiger_timer, period);
    }
}

void ble_spam_scene_tracker_geiger_on_enter(void* context) {
    BleSpamApp* app = context;

    TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
    memset(model, 0, sizeof(TrackerGeigerModel));
    memcpy(model->target_addr, app->tracker_target.addr, 6);
    model->target_kind = app->tracker_target.kind;
    strncpy(model->target_name, app->tracker_target.name, sizeof(model->target_name) - 1);
    model->target_name[sizeof(model->target_name) - 1] = '\0';
    model->rssi = app->tracker_target.rssi;
    model->rssi_smooth = app->tracker_target.rssi;
    model->rssi_peak = app->tracker_target.rssi;
    model->trend = 0;
    model->last_seen_ms = furi_get_tick();
    model->stale = false;
    view_commit_model(app->view_tracker_geiger, true);

    app->tracker_current_rssi = app->tracker_target.rssi;
    app->tracker_current_stale = false;
    app->tracker_current_period = 1200;

    s_have_prev = false;
    s_peak_decay_ms = furi_get_tick();

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewTrackerGeiger);

    // Re-arm scanner — the scan scene stopped it in its on_exit
    ble_tracker_hal_start_scan();

    // NOTE: do NOT acquire the speaker here — the timer callback acquires it on its
    // own thread per beep (the mutex is owner-per-thread). Holding it on this scene
    // thread would block the timer thread from ever acquiring it.

    app->tracker_geiger_timer =
        furi_timer_alloc(tracker_geiger_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->tracker_geiger_timer, 1200);
}

bool ble_spam_scene_tracker_geiger_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
            model->rssi_peak = model->rssi_smooth;   // clear peak for a fresh sweep
            view_commit_model(app->view_tracker_geiger, true);
            s_peak_decay_ms = furi_get_tick();
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        TrackerDevice* devices = ble_tracker_hal_get_devices(&count);

        // Find target by MAC
        TrackerDevice* match = NULL;
        for(uint16_t i = 0; i < count; i++) {
            if(memcmp(devices[i].addr, app->tracker_target.addr, 6) == 0) {
                match = &devices[i];
                break;
            }
        }

        TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
        uint32_t now = furi_get_tick();

        if(match) {
            model->rssi = match->rssi;
            model->last_seen_ms = match->last_seen_ms;
        }
        bool stale = (now - model->last_seen_ms) > STALE_TIMEOUT_MS;
        model->stale = stale;

        // EMA smoothing: smooth = (2*smooth + raw)/3
        int32_t sm = ((int32_t)model->rssi_smooth * 2 + (int32_t)model->rssi) / 3;
        model->rssi_smooth = (int8_t)sm;

        // Trend from the smoothed delta (+/-1 dB threshold)
        if(s_have_prev) {
            int d = (int)model->rssi_smooth - (int)s_prev_smooth;
            model->trend = (d >= 1) ? 1 : (d <= -1 ? -1 : 0);
        } else {
            model->trend = 0;
        }
        s_prev_smooth = model->rssi_smooth;
        s_have_prev = true;

        // Peak-hold: instant rise, ~1 dB/s decay toward the current smoothed value
        if(model->rssi_smooth > model->rssi_peak) {
            model->rssi_peak = model->rssi_smooth;
        } else if((now - s_peak_decay_ms) >= 1000) {
            if(model->rssi_peak > model->rssi_smooth) model->rssi_peak--;
            s_peak_decay_ms = now;
        }

        view_commit_model(app->view_tracker_geiger, true);

        // Mirror to app state for timer-thread access
        app->tracker_current_rssi = model->rssi_smooth;
        app->tracker_current_stale = stale;
    }

    return consumed;
}

void ble_spam_scene_tracker_geiger_on_exit(void* context) {
    BleSpamApp* app = context;

    if(app->tracker_geiger_timer) {
        furi_timer_stop(app->tracker_geiger_timer);
        furi_timer_free(app->tracker_geiger_timer);
        app->tracker_geiger_timer = NULL;
    }

    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }

    ble_tracker_hal_stop_scan();
    ble_tracker_hal_release();
}
