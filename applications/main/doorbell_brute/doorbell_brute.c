#include "doorbell_brute.h"
#include "doorbell_codes.h"
#include "doorbell_tristate.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification_messages.h>
#include <furi_hal_subghz.h>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "../subghz/helpers/subbrute_worker.h"
#include "../subghz/helpers/subbrute_protocols.h"
#include "../subghz/helpers/subbrute_radio_device_loader.h"
#include <lib/subghz/devices/devices.h>

#define TAG "DoorbellBrute"

// Real doorbells are Princeton/PT2262-family fixed codes: 12 tri-state symbols
// (each pin 0/1/float), transmitted as PWM symbols + a per-word sync gap (NOT raw
// NRZ — that's why the earlier de Bruijn sweep never rang a bell). This app:
//   Phase 0 — replay a table of known doorbell codes (doorbell_codes.h) as framed
//             Princeton; rings known bells (incl. non-tri-state EV1527) instantly.
//   Phase 1 — a REAL tri-state ADDRESS brute: enumerate the 3^8 = 6561 PT2262
//             address combinations (doorbell_tristate.h), each a framed Princeton
//             packet, over a small data/te/freq set — rings ANY DIP-switch bell
//             whose address the sweep hits. This is feasible (minutes) because the
//             code space is 3^8, not 2^24.
// Both phases use the same framed-Princeton TX the Sub-GHz app uses to replay a
// saved .sub, so a real bell decodes and rings.

#define DB_FREQ_433 433920000u
#define DB_FREQ_315 315000000u

#define REPEATS_MIN 1 // Princeton frames back-to-back per candidate
#define REPEATS_MAX 5

typedef enum {
    DbFreq433 = 0,
    DbFreq315,
    DbFreqBoth,
    DbFreqCount,
} DbFreqSel;

typedef enum {
    DbViewConfig,
    DbViewStatus,
} DbView;

typedef enum {
    DbPhaseKnown = 0, // Phase 0: known-code DB replay
    DbPhaseTristate, // Phase 1: 3^8 tri-state address brute
} DbPhase;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    VariableItemList* vil;
    View* status;

    const SubGhzDevice* radio;
    SubBruteWorker* worker;
    SubBruteProtocol cur_proto; // rebuilt per code so its lifetime spans the TX

    FuriThread* sweep_thread;
    volatile bool running; // clear to request the sweep stop
    volatile bool done; // set by the sweep thread when it finishes on its own
    bool thread_active; // a sweep thread has been started and not yet joined

    // live progress (read by the status view on the GUI thread)
    volatile uint8_t phase; // DbPhase
    volatile uint16_t cur_index; // Phase 0: code index
    volatile uint32_t cur_n; // Phase 1: tri-state address index
    volatile uint16_t cur_te; // Phase 1: current te
    volatile uint8_t cur_data; // Phase 1: current data byte
    volatile uint32_t cur_freq; // Phase 1: current frequency
    volatile uint8_t cur_block; // Phase 1: freq/te/data block (1-based)
    uint8_t total_blocks; // Phase 1: freq_count * DB_TE_COUNT * DB_DATA_COUNT

    // config
    uint8_t freq_sel; // DbFreqSel
    uint8_t repeats; // frames per candidate

    // resolved frequency list for the run
    uint32_t freqs[2];
    uint8_t freq_count;

    bool on_status;
} DoorbellApp;

typedef struct {
    uint8_t phase;
    uint16_t idx; // 1-based within phase
    uint32_t idx_max; // codes (Phase 0) or 6561 (Phase 1)
    uint8_t block;
    uint8_t blocks;
    uint16_t te;
    uint8_t data;
    uint32_t freq;
    char name[24]; // Phase 0 code label
    bool running;
    bool done;
} DbStatusModel;

// ---- status view ------------------------------------------------------------

static void db_status_draw(Canvas* canvas, void* model) {
    DbStatusModel* m = model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Doorbell Brute");

    canvas_set_font(canvas, FontSecondary);

    char l1[36];
    if(m->phase == DbPhaseKnown) {
        snprintf(l1, sizeof(l1), "Known codes  %u/%lu", m->idx, (unsigned long)m->idx_max);
    } else {
        snprintf(l1, sizeof(l1), "Tri-state  %lu/%lu", (unsigned long)m->idx, (unsigned long)m->idx_max);
    }
    canvas_draw_str(canvas, 2, 21, l1);

    char l2[36];
    if(m->phase == DbPhaseKnown) {
        snprintf(l2, sizeof(l2), "%s", m->name);
    } else {
        snprintf(
            l2,
            sizeof(l2),
            "%luM te%u d%02X  blk %u/%u",
            (unsigned long)(m->freq / 1000000u),
            m->te,
            m->data,
            m->block,
            m->blocks);
    }
    canvas_draw_str(canvas, 2, 32, l2);

    // Overall progress: Phase 0 is the first sliver, Phase 1 fills the rest.
    float p;
    if(m->phase == DbPhaseKnown) {
        p = (m->idx_max > 0) ? (float)m->idx / (float)(m->idx_max * 8) : 0.0f; // ~1/8 weight
    } else {
        float blk = (m->blocks > 0) ? (float)(m->block - 1) / (float)m->blocks : 0.0f;
        float within = (m->idx_max > 0) ? (float)m->idx / (float)(m->idx_max * m->blocks) : 0.0f;
        p = 0.10f + 0.90f * (blk + within);
    }
    if(m->done) p = 1.0f;
    if(p > 1.0f) p = 1.0f;
    if(p < 0.0f) p = 0.0f; // block==0 at Phase-1 entry makes (block-1) underflow negative
    elements_progress_bar(canvas, 2, 46, 124, p);

    canvas_draw_str(canvas, 2, 62, m->done ? "Done  (Back)" : "Back: stop");
}

static bool db_status_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false; // let the view dispatcher's navigation handle Back
}

static void db_status_update(DoorbellApp* app) {
    with_view_model(
        app->status,
        DbStatusModel * m,
        {
            m->phase = app->phase;
            m->running = app->running;
            m->done = app->done;
            if(app->phase == DbPhaseKnown) {
                uint16_t idx = app->cur_index;
                if(idx >= DOORBELL_CODE_COUNT) idx = DOORBELL_CODE_COUNT - 1;
                strncpy(m->name, kDoorbellCodes[idx].name, sizeof(m->name) - 1);
                m->name[sizeof(m->name) - 1] = '\0';
                m->idx = (uint16_t)(idx + 1);
                m->idx_max = DOORBELL_CODE_COUNT;
            } else {
                m->idx = (uint16_t)(app->cur_n + 1);
                m->idx_max = DB_TRISTATE_COUNT;
                m->block = app->cur_block;
                m->blocks = app->total_blocks;
                m->te = app->cur_te;
                m->data = app->cur_data;
                m->freq = app->cur_freq;
            }
        },
        true);
}

// ---- transmit ---------------------------------------------------------------

// Transmit one 24-bit Princeton code as a framed packet at (freq, te, repeat).
// load_index=7 + step=key&0xFF reconstructs the EXACT key with no byte brute-
// forced; clear_tx_guard skips the worker's 250ms manual-TX floor so the sweep
// runs back-to-back (that floor alone would be ~27 min over 6561 codes).
static void db_tx_word(DoorbellApp* app, uint32_t freq, uint32_t key, uint16_t te, uint8_t repeat) {
    app->cur_proto = (SubBruteProtocol){
        .frequency = freq,
        .bits = 24,
        .te = te,
        .repeat = repeat,
        .opencode = 0,
        .preset = FuriHalSubGhzPresetOok650Async,
        .file = PrincetonFileProtocol,
    };
    if(!subbrute_worker_init_file_attack(
           app->worker, 0, 7, (uint64_t)key, &app->cur_proto, repeat, false)) {
        return;
    }
    subbrute_worker_clear_tx_guard(app->worker);
    subbrute_worker_transmit_current_key(app->worker, (uint64_t)(key & 0xFFu));
}

// ---- sweep thread -----------------------------------------------------------

static int32_t db_sweep_thread(void* ctx) {
    DoorbellApp* app = ctx;

    // Phase 0: known-code DB — fast, rings known/non-tri-state bells immediately.
    app->phase = DbPhaseKnown;
    for(uint16_t i = 0; app->running && i < DOORBELL_CODE_COUNT; i++) {
        app->cur_index = i;
        const DoorbellCode* c = &kDoorbellCodes[i];
        db_tx_word(app, c->freq, c->key, c->te, app->repeats);
    }

    // Phase 1: 3^8 tri-state address brute over freq x te x data.
    app->phase = DbPhaseTristate;
    uint8_t block = 0;
    for(uint8_t fi = 0; app->running && fi < app->freq_count; fi++) {
        for(uint8_t ti = 0; app->running && ti < DB_TE_COUNT; ti++) {
            for(uint8_t di = 0; app->running && di < DB_DATA_COUNT; di++) {
                block++;
                app->cur_block = block;
                app->cur_freq = app->freqs[fi];
                app->cur_te = kDbTeSet[ti];
                app->cur_data = kDbDataSet[di];
                for(uint32_t n = 0; app->running && n < DB_TRISTATE_COUNT; n++) {
                    app->cur_n = n;
                    uint32_t word = db_tristate_word(n, kDbDataSet[di]);
                    db_tx_word(app, app->freqs[fi], word, kDbTeSet[ti], app->repeats);
                }
            }
        }
    }

    app->running = false;
    app->done = true;
    return 0;
}

// ---- brute control ----------------------------------------------------------

static void db_stop(DoorbellApp* app) {
    if(app->thread_active) {
        app->running = false;
        furi_thread_join(app->sweep_thread);
        app->thread_active = false;
        notification_message(app->notifications, &sequence_blink_stop);
    }
}

static void db_resolve_freqs(DoorbellApp* app) {
    switch(app->freq_sel) {
    case DbFreq315:
        app->freqs[0] = DB_FREQ_315;
        app->freq_count = 1;
        break;
    case DbFreqBoth:
        app->freqs[0] = DB_FREQ_433;
        app->freqs[1] = DB_FREQ_315;
        app->freq_count = 2;
        break;
    case DbFreq433:
    default:
        app->freqs[0] = DB_FREQ_433;
        app->freq_count = 1;
        break;
    }
    app->total_blocks = (uint8_t)(app->freq_count * DB_TE_COUNT * DB_DATA_COUNT);
}

static void db_start(DoorbellApp* app) {
    db_stop(app); // ensure no prior sweep is still active
    db_resolve_freqs(app);
    app->running = true;
    app->done = false;
    app->phase = DbPhaseKnown;
    app->cur_index = 0;
    app->cur_n = 0;
    app->cur_block = 0;

    app->on_status = true;
    db_status_update(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, DbViewStatus);

    furi_thread_start(app->sweep_thread);
    app->thread_active = true;
    notification_message(app->notifications, &sequence_single_vibro);
    notification_message(app->notifications, &sequence_blink_start_magenta);
}

// ---- config list ------------------------------------------------------------

static const char* const kFreqLabels[DbFreqCount] = {"433.92", "315", "Both"};

static void db_freq_changed(VariableItem* item) {
    DoorbellApp* app = variable_item_get_context(item);
    uint8_t v = variable_item_get_current_value_index(item);
    app->freq_sel = v;
    variable_item_set_current_value_text(item, kFreqLabels[v]);
}

static void db_repeats_changed(VariableItem* item) {
    DoorbellApp* app = variable_item_get_context(item);
    uint8_t v = variable_item_get_current_value_index(item) + REPEATS_MIN;
    app->repeats = v;
    char t[8];
    snprintf(t, sizeof(t), "%u", v);
    variable_item_set_current_value_text(item, t);
}

// index 0 = Frequency, 1 = Repeats, 2 = Start
static void db_config_enter(void* context, uint32_t index) {
    DoorbellApp* app = context;
    if(index == 2) {
        db_start(app);
    }
}

// ---- callbacks --------------------------------------------------------------

static void db_tick(void* context) {
    DoorbellApp* app = context;
    if(!app->on_status) return;

    // The sweep thread finished on its own — reap it and signal completion once.
    if(app->done && app->thread_active) {
        furi_thread_join(app->sweep_thread);
        app->thread_active = false;
        notification_message(app->notifications, &sequence_double_vibro);
        notification_message(app->notifications, &sequence_blink_stop);
    }
    db_status_update(app);
}

static bool db_nav(void* context) {
    DoorbellApp* app = context;
    if(app->on_status) {
        db_stop(app);
        app->on_status = false;
        view_dispatcher_switch_to_view(app->view_dispatcher, DbViewConfig);
        return true; // consumed — stay in the app
    }
    view_dispatcher_stop(app->view_dispatcher); // exit from config
    return true;
}

// ---- app --------------------------------------------------------------------

int32_t doorbell_brute_app(void* p) {
    UNUSED(p);

    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < (24u * 1024u)) {
        FURI_LOG_E(TAG, "not enough heap for Doorbell Brute");
        return 0;
    }

    DoorbellApp* app = malloc(sizeof(DoorbellApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    memset(app, 0, sizeof(DoorbellApp));
    app->repeats = 2;
    app->freq_sel = DbFreq433;

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Register the SubGhz radio devices (CC1101 int/ext). The SubBrute radio
    // loader does subghz_devices_get_by_name() + furi_assert() — without this the
    // lookup returns NULL and the app crash-reboots on open.
    subghz_devices_init();
    app->radio = subbrute_radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);
    app->worker = subbrute_worker_alloc(app->radio);

    app->sweep_thread = furi_thread_alloc();
    furi_thread_set_name(app->sweep_thread, "DoorbellSweep");
    furi_thread_set_stack_size(app->sweep_thread, 4096);
    furi_thread_set_context(app->sweep_thread, app);
    furi_thread_set_callback(app->sweep_thread, db_sweep_thread);

    app->view_dispatcher = view_dispatcher_alloc();
    app->vil = variable_item_list_alloc();
    app->status = view_alloc();
    view_allocate_model(app->status, ViewModelTypeLocking, sizeof(DbStatusModel));
    view_set_draw_callback(app->status, db_status_draw);
    view_set_input_callback(app->status, db_status_input);

    VariableItem* it;
    it = variable_item_list_add(app->vil, "Frequency", DbFreqCount, db_freq_changed, app);
    variable_item_set_current_value_index(it, app->freq_sel);
    variable_item_set_current_value_text(it, kFreqLabels[app->freq_sel]);

    it = variable_item_list_add(
        app->vil, "Repeats", REPEATS_MAX - REPEATS_MIN + 1, db_repeats_changed, app);
    variable_item_set_current_value_index(it, app->repeats - REPEATS_MIN);
    {
        char t[8];
        snprintf(t, sizeof(t), "%u", app->repeats);
        variable_item_set_current_value_text(it, t);
    }

    variable_item_list_add(app->vil, "Start", 0, NULL, app);
    variable_item_list_set_enter_callback(app->vil, db_config_enter, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, db_nav);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, db_tick, 200);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(
        app->view_dispatcher, DbViewConfig, variable_item_list_get_view(app->vil));
    view_dispatcher_add_view(app->view_dispatcher, DbViewStatus, app->status);
    view_dispatcher_switch_to_view(app->view_dispatcher, DbViewConfig);

    view_dispatcher_run(app->view_dispatcher);

    db_stop(app);
    view_dispatcher_remove_view(app->view_dispatcher, DbViewStatus);
    view_dispatcher_remove_view(app->view_dispatcher, DbViewConfig);
    view_free(app->status);
    variable_item_list_free(app->vil);
    view_dispatcher_free(app->view_dispatcher);

    furi_thread_free(app->sweep_thread);
    // subbrute_worker_free() already ends the radio device loader, so do NOT call
    // subbrute_radio_device_loader_end(app->radio) again here (double-end would
    // subghz_devices_end() the external CC1101 twice).
    if(app->worker) subbrute_worker_free(app->worker);
    subghz_devices_deinit();

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
