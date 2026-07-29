#include "../can_commander_app_i.h"
#include <stdio.h>

static const uint32_t can_intervals[] = {10, 20, 50, 100, 500};
static const char* const can_interval_txt[] = {"10ms", "20ms", "50ms", "100ms", "500ms"};

#define ROW_INTERVAL 0
#define ROW_FRAME 1
#define ROW_ACTION 2

// Single-instance app -> a file-static handle to the action row is fine.
static VariableItem* s_action_item;

static uint8_t can_interval_index(uint32_t ms) {
    for(uint8_t i = 0; i < 5; i++)
        if(can_intervals[i] == ms) return i;
    return 3; // 100ms
}

static int32_t can_periodic_worker(void* context) {
    CanCommanderApp* app = context;
    while(app->tx_run) {
        can_hal_transmit(&app->tx_frame, 20);
        app->tx_count++;
        uint32_t waited = 0;
        // sleep in small steps so Stop is responsive
        while(app->tx_run && waited < app->tx_interval_ms) {
            uint32_t step = app->tx_interval_ms - waited;
            if(step > 20) step = 20;
            furi_delay_ms(step);
            waited += step;
        }
    }
    return 0;
}

static void can_periodic_interval_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->tx_interval_ms = can_intervals[idx];
    variable_item_set_current_value_text(item, can_interval_txt[idx]);
}

static void can_periodic_stop(CanCommanderApp* app) {
    if(app->tx_thread) {
        app->tx_run = false;
        furi_thread_join(app->tx_thread);
        furi_thread_free(app->tx_thread);
        app->tx_thread = NULL;
    }
}

static void can_periodic_enter_cb(void* context, uint32_t index) {
    CanCommanderApp* app = context;
    if(index != ROW_ACTION) return;

    if(app->tx_run) {
        can_periodic_stop(app);
        if(s_action_item) variable_item_set_current_value_text(s_action_item, "Stopped");
        return;
    }

    // Start: need a valid frame + a running driver.
    if(!can_commander_parse_input(app->frame_input, &app->tx_frame)) {
        if(s_action_item) variable_item_set_current_value_text(s_action_item, "Bad frame");
        return;
    }
    if(!can_hal_is_running()) {
        CanHalConfig cfg = {
            .bitrate = app->bitrate,
            .mode = app->mode,
            .tx_gpio = app->tx_gpio,
            .rx_gpio = app->rx_gpio};
        if(!can_hal_start(&cfg)) {
            if(s_action_item) variable_item_set_current_value_text(s_action_item, "Init fail");
            return;
        }
    }
    app->tx_count = 0;
    app->tx_run = true;
    app->tx_thread = furi_thread_alloc_ex("CanTx", 2048, can_periodic_worker, app);
    furi_thread_start(app->tx_thread);
    if(s_action_item) variable_item_set_current_value_text(s_action_item, "Running");
}

void can_commander_scene_periodic_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    VariableItemList* vil = app->var_item_list;
    variable_item_list_reset(vil);
    if(app->tx_interval_ms == 0) app->tx_interval_ms = 100;

    VariableItem* it;
    uint8_t ii = can_interval_index(app->tx_interval_ms);
    it = variable_item_list_add(vil, "Interval", 5, can_periodic_interval_changed, app);
    variable_item_set_current_value_index(it, ii);
    variable_item_set_current_value_text(it, can_interval_txt[ii]);

    it = variable_item_list_add(vil, "Frame", 1, NULL, app);
    variable_item_set_current_value_text(it, app->frame_input);

    s_action_item = variable_item_list_add(vil, "OK: Start/Stop", 1, NULL, app);
    variable_item_set_current_value_text(s_action_item, app->tx_run ? "Running" : "Stopped");

    variable_item_list_set_enter_callback(vil, can_periodic_enter_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewVarItemList);
}

bool can_commander_scene_periodic_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(event.type == SceneManagerEventTypeTick && app->tx_run && s_action_item) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Run %lu", (unsigned long)app->tx_count);
        variable_item_set_current_value_text(s_action_item, buf);
        return true;
    }
    return false;
}

void can_commander_scene_periodic_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    can_periodic_stop(app);
    can_hal_stop();
    s_action_item = NULL;
    variable_item_list_reset(app->var_item_list);
}
