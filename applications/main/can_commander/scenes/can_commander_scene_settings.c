#include "../can_commander_app_i.h"

static const uint32_t can_bitrates[] = {125000, 250000, 500000, 1000000};
static const char* const can_bitrate_txt[] = {"125k", "250k", "500k", "1M"};
static const char* const can_mode_txt[] = {"Normal", "No-ACK", "Listen"};
static const char* const can_swap_txt[] = {"No", "Yes"};

static uint8_t can_bitrate_index(uint32_t b) {
    for(uint8_t i = 0; i < 4; i++)
        if(can_bitrates[i] == b) return i;
    return 2; // 500k
}

static void can_settings_bitrate_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->bitrate = can_bitrates[idx];
    variable_item_set_current_value_text(item, can_bitrate_txt[idx]);
}

static void can_settings_mode_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->mode = (CanMode)idx;
    variable_item_set_current_value_text(item, can_mode_txt[idx]);
}

static void can_settings_swap_changed(VariableItem* item) {
    CanCommanderApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx == 0) {
        app->tx_gpio = 2;
        app->rx_gpio = 1;
    } else {
        app->tx_gpio = 1;
        app->rx_gpio = 2;
    }
    variable_item_set_current_value_text(item, can_swap_txt[idx]);
}

void can_commander_scene_settings_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    VariableItemList* vil = app->var_item_list;
    variable_item_list_reset(vil);

    VariableItem* it;
    uint8_t bi = can_bitrate_index(app->bitrate);
    it = variable_item_list_add(vil, "Bitrate", 4, can_settings_bitrate_changed, app);
    variable_item_set_current_value_index(it, bi);
    variable_item_set_current_value_text(it, can_bitrate_txt[bi]);

    it = variable_item_list_add(vil, "Mode", 3, can_settings_mode_changed, app);
    variable_item_set_current_value_index(it, (uint8_t)app->mode);
    variable_item_set_current_value_text(it, can_mode_txt[(uint8_t)app->mode]);

    uint8_t swi = (app->tx_gpio == 1) ? 1 : 0;
    it = variable_item_list_add(vil, "TX/RX Swap", 2, can_settings_swap_changed, app);
    variable_item_set_current_value_index(it, swi);
    variable_item_set_current_value_text(it, can_swap_txt[swi]);

    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewVarItemList);
}

bool can_commander_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void can_commander_scene_settings_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
