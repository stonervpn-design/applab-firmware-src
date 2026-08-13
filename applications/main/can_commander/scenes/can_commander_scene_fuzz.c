#include "../can_commander_app_i.h"
#include "../helpers/can_fuzz.h"
#include <stdio.h>
#include <string.h>

// CAN-bus fuzzer scene. Clones can_commander_scene_periodic.c's FuriThread
// worker / stepped-sleep abort / Tick-driven status shape, driving
// can_fuzz_next() -> can_hal_transmit() instead of a fixed replay frame.
//
// Honest by design: the on-screen "Bus" row is can_hal_stats() verbatim
// (real TWAI tx_ok/tx_fail/bus_err/bus_off counters). A no-ACK / no bus_err
// can simply mean nothing is listening on the bus -- this is not, and never
// claims to be, a crash/vuln verdict.

static const char* const can_fuzz_mode_txt[CFZ_MODE_COUNT] = {
    "BitFlip",
    "DataRnd",
    "IDSweep",
    "DLCSweep",
    "FullRnd",
};

// TX rate, same set as can_commander_scene_periodic.c. The worker reads
// app->tx_interval_ms (shared with Periodic); this row lets the operator pick
// fast-flood vs slow-probe from within the fuzzer.
static const uint32_t can_fuzz_intervals[] = {10, 20, 50, 100, 500};
static const char* const can_fuzz_interval_txt[] = {"10ms", "20ms", "50ms", "100ms", "500ms"};

static uint8_t can_fuzz_interval_index(uint32_t ms) {
    for(uint8_t i = 0; i < 5; i++)
        if(can_fuzz_intervals[i] == ms) return i;
    return 3; // default 100ms
}

#define ROW_MODE 0
#define ROW_INTERVAL 1
#define ROW_ACTION 2
#define ROW_STATS 3
#define ROW_INFO 4

// Single-instance app -> file-static handles/state are fine (same convention
// as can_commander_scene_periodic.c's s_action_item).
static VariableItem* s_fuzz_action_item;
static VariableItem* s_fuzz_stats_item;
static CanFrame s_fuzz_base;
static uint32_t s_fuzz_seed;
static bool s_fuzz_bus_off_handled;

static void can_fuzz_pick_base(CanCommanderApp* app, CanFrame* base) {
    if(app->tx_frame.id != 0 || app->tx_frame.dlc != 0) {
        *base = app->tx_frame;
        return;
    }
    memset(base, 0, sizeof(*base));
    base->id = 0x100;
    base->extended = false;
    base->rtr = false;
    base->dlc = 8;
    for(uint8_t i = 0; i < 8; i++) base->data[i] = i;
}

static int32_t can_fuzz_worker(void* context) {
    CanCommanderApp* app = context;
    uint32_t step = 0;
    while(app->fuzz_run) {
        CanFrame f;
        can_fuzz_next(&s_fuzz_base, (CanFuzzMode)app->fuzz_mode, s_fuzz_seed, step++, &f);
        if(can_hal_transmit(&f, 20)) app->fuzz_count++;
        uint32_t waited = 0;
        // sleep in small steps so Stop is responsive
        while(app->fuzz_run && waited < app->tx_interval_ms) {
            uint32_t s = app->tx_interval_ms - waited;
            if(s > 20) s = 20;
            furi_delay_ms(s);
            waited += s;
        }
    }
    return 0;
}

static void can_fuzz_mode_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->fuzz_mode = idx;
    variable_item_set_current_value_text(item, can_fuzz_mode_txt[idx]);
}

static void can_fuzz_interval_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->tx_interval_ms = can_fuzz_intervals[idx];
    variable_item_set_current_value_text(item, can_fuzz_interval_txt[idx]);
}

static void can_fuzz_stop(CanCommanderApp* app) {
    if(app->fuzz_thread) {
        app->fuzz_run = false;
        furi_thread_join(app->fuzz_thread);
        furi_thread_free(app->fuzz_thread);
        app->fuzz_thread = NULL;
    }
}

static void can_fuzz_enter_cb(void* context, uint32_t index) {
    CanCommanderApp* app = context;
    if(index != ROW_ACTION) return;

    if(app->fuzz_run) {
        can_fuzz_stop(app);
        if(s_fuzz_action_item) variable_item_set_current_value_text(s_fuzz_action_item, "Stopped");
        return;
    }

    // Start: need a running driver (no frame-string parse needed here --
    // the base is either the shared tx_frame or a built-in default).
    if(!can_hal_is_running()) {
        CanHalConfig cfg = {
            .bitrate = app->bitrate,
            .mode = app->mode,
            .tx_gpio = app->tx_gpio,
            .rx_gpio = app->rx_gpio};
        if(!can_hal_start(&cfg)) {
            if(s_fuzz_action_item)
                variable_item_set_current_value_text(s_fuzz_action_item, "Init fail");
            return;
        }
    }
    can_fuzz_pick_base(app, &s_fuzz_base);
    s_fuzz_seed = furi_get_tick();
    s_fuzz_bus_off_handled = false;
    app->fuzz_count = 0;
    app->fuzz_run = true;
    app->fuzz_thread = furi_thread_alloc_ex("CanFuzz", 2048, can_fuzz_worker, app);
    furi_thread_start(app->fuzz_thread);
    if(s_fuzz_action_item) variable_item_set_current_value_text(s_fuzz_action_item, "Running");
}

void can_commander_scene_fuzz_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    VariableItemList* vil = app->var_item_list;
    variable_item_list_reset(vil);
    if(app->tx_interval_ms == 0) app->tx_interval_ms = 100;

    VariableItem* it;
    it = variable_item_list_add(vil, "Mode", CFZ_MODE_COUNT, can_fuzz_mode_changed, app);
    variable_item_set_current_value_index(it, app->fuzz_mode);
    variable_item_set_current_value_text(it, can_fuzz_mode_txt[app->fuzz_mode]);

    uint8_t ii = can_fuzz_interval_index(app->tx_interval_ms);
    it = variable_item_list_add(vil, "Interval", 5, can_fuzz_interval_changed, app);
    variable_item_set_current_value_index(it, ii);
    variable_item_set_current_value_text(it, can_fuzz_interval_txt[ii]);

    s_fuzz_action_item = variable_item_list_add(vil, "OK: Start/Stop", 1, NULL, app);
    variable_item_set_current_value_text(s_fuzz_action_item, app->fuzz_run ? "Running" : "Stopped");

    s_fuzz_stats_item = variable_item_list_add(vil, "Bus", 1, NULL, app);
    variable_item_set_current_value_text(s_fuzz_stats_item, "Sent 0 fail 0 err 0");

    it = variable_item_list_add(vil, "Info", 1, NULL, app);
    variable_item_set_current_value_text(
        it, "Bus state=real TWAI (no-ACK may=no device). Not a crash verdict.");

    variable_item_list_set_enter_callback(vil, can_fuzz_enter_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewVarItemList);
}

bool can_commander_scene_fuzz_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !app->fuzz_run) return false;

    if(s_fuzz_action_item) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Run %lu", (unsigned long)app->fuzz_count);
        variable_item_set_current_value_text(s_fuzz_action_item, buf);
    }

    CanHalStats st = can_hal_stats();
    if(st.bus_off) {
        if(!s_fuzz_bus_off_handled) {
            can_hal_recover();
            s_fuzz_bus_off_handled = true;
        }
    } else {
        s_fuzz_bus_off_handled = false;
    }
    if(s_fuzz_stats_item) {
        char buf[40];
        snprintf(
            buf,
            sizeof(buf),
            "Sent %lu fail %lu err %lu%s",
            (unsigned long)st.tx_ok,
            (unsigned long)st.tx_fail,
            (unsigned long)st.bus_err,
            st.bus_off ? " BUS-OFF" : "");
        variable_item_set_current_value_text(s_fuzz_stats_item, buf);
    }
    return true;
}

void can_commander_scene_fuzz_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    can_fuzz_stop(app);
    can_hal_stop();
    s_fuzz_action_item = NULL;
    s_fuzz_stats_item = NULL;
    variable_item_list_reset(app->var_item_list);
}
