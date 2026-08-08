#include "../subghz_i.h"

// TPMS Spoofer config (PRIVATE-only): set sensor ID / pressure / temperature /
// frequency for the picked protocol, then Transmit -> seed the synthetic frame
// and hand it to the Transmitter. Reached from subghz_scene_tpms_spoof (the
// protocol picker), which stashes the proto index in this scene's state.
#ifndef PUBLIC_BUILD

#include "../helpers/tpms_spoof.h"

#define PRESSURE_N 26 // 1.0 .. 3.5 bar, 0.1 steps
#define TEMP_N     17 // -20 .. 60 C, 5 deg steps
#define FREQ_N     2

typedef enum {
    RowPressure = 0,
    RowTemp,
    RowFreq,
    RowId,
    RowTransmit,
} TpmsSpoofRow;

#define EVT_ID_DONE 0xF001u

static const uint32_t k_freqs[FREQ_N] = {433920000, 315000000};
static const char* const k_freq_txt[FREQ_N] = {"433.92", "315"};

// Single active config session — working values kept file-static.
static int s_active_proto = -1;
static uint8_t s_pressure_idx = 12; // 2.2 bar
static uint8_t s_temp_idx = 8; // 20 C
static uint8_t s_freq_idx = 0; // 433.92

static float cfg_pressure(void) {
    return 1.0f + (float)s_pressure_idx * 0.1f;
}
static int cfg_temp(void) {
    return -20 + (int)s_temp_idx * 5;
}
static uint32_t cfg_id(SubGhz* subghz) {
    return ((uint32_t)subghz->tpms_edit_id_bytes[0] << 24) |
           ((uint32_t)subghz->tpms_edit_id_bytes[1] << 16) |
           ((uint32_t)subghz->tpms_edit_id_bytes[2] << 8) |
           ((uint32_t)subghz->tpms_edit_id_bytes[3]);
}

static void pressure_cb(VariableItem* item) {
    s_pressure_idx = variable_item_get_current_value_index(item);
    char b[16];
    snprintf(b, sizeof(b), "%d.%d bar", (10 + s_pressure_idx) / 10, (10 + s_pressure_idx) % 10);
    variable_item_set_current_value_text(item, b);
}
static void temp_cb(VariableItem* item) {
    s_temp_idx = variable_item_get_current_value_index(item);
    char b[16];
    snprintf(b, sizeof(b), "%d C", cfg_temp());
    variable_item_set_current_value_text(item, b);
}
static void freq_cb(VariableItem* item) {
    s_freq_idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, k_freq_txt[s_freq_idx]);
}

static void tpms_spoof_config_enter_cb(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}
static void tpms_spoof_config_id_done_cb(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, EVT_ID_DONE);
}

static void tpms_spoof_config_build(SubGhz* subghz) {
    VariableItemList* vil = subghz->variable_item_list;
    variable_item_list_reset(vil);
    char b[20];
    VariableItem* it;

    it = variable_item_list_add(vil, "Pressure", PRESSURE_N, pressure_cb, subghz);
    variable_item_set_current_value_index(it, s_pressure_idx);
    snprintf(b, sizeof(b), "%d.%d bar", (10 + s_pressure_idx) / 10, (10 + s_pressure_idx) % 10);
    variable_item_set_current_value_text(it, b);

    it = variable_item_list_add(vil, "Temp", TEMP_N, temp_cb, subghz);
    variable_item_set_current_value_index(it, s_temp_idx);
    snprintf(b, sizeof(b), "%d C", cfg_temp());
    variable_item_set_current_value_text(it, b);

    it = variable_item_list_add(vil, "Freq (MHz)", FREQ_N, freq_cb, subghz);
    variable_item_set_current_value_index(it, s_freq_idx);
    variable_item_set_current_value_text(it, k_freq_txt[s_freq_idx]);

    it = variable_item_list_add(vil, "Sensor ID", 1, NULL, subghz);
    snprintf(b, sizeof(b), "%08lX", (unsigned long)cfg_id(subghz));
    variable_item_set_current_value_text(it, b);

    variable_item_list_add(vil, "Transmit", 0, NULL, subghz);

    variable_item_list_set_enter_callback(vil, tpms_spoof_config_enter_cb, subghz);
}

void subghz_scene_tpms_spoof_config_on_enter(void* context) {
    SubGhz* subghz = context;
    int proto =
        (int)scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneTpmsSpoofConfig);
    const TpmsSpoofProto* p = tpms_spoof_proto_get((size_t)proto);

    // Fresh protocol pick -> seed the working values from that protocol's defaults.
    // Re-entry (e.g. back from the Transmitter) keeps the operator's edits.
    if(p && s_active_proto != proto) {
        s_active_proto = proto;
        int pi = (int)((p->default_pressure_bar - 1.0f) / 0.1f + 0.5f);
        s_pressure_idx = (uint8_t)(pi < 0 ? 0 : (pi >= PRESSURE_N ? PRESSURE_N - 1 : pi));
        int ti = (int)((p->default_temperature_c + 20.0f) / 5.0f + 0.5f);
        s_temp_idx = (uint8_t)(ti < 0 ? 0 : (ti >= TEMP_N ? TEMP_N - 1 : ti));
        s_freq_idx = 0;
        subghz->tpms_edit_id_bytes[0] = (p->default_id >> 24) & 0xFF;
        subghz->tpms_edit_id_bytes[1] = (p->default_id >> 16) & 0xFF;
        subghz->tpms_edit_id_bytes[2] = (p->default_id >> 8) & 0xFF;
        subghz->tpms_edit_id_bytes[3] = (p->default_id >> 0) & 0xFF;
    }

    tpms_spoof_config_build(subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_tpms_spoof_config_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == EVT_ID_DONE) {
        // Returned from the hex ID editor — rebuild the list to show the new ID.
        tpms_spoof_config_build(subghz);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
        return true;
    }
    if(event.event == RowId) {
        byte_input_set_header_text(subghz->byte_input, "Sensor ID (hex)");
        byte_input_set_result_callback(
            subghz->byte_input,
            tpms_spoof_config_id_done_cb,
            NULL,
            subghz,
            subghz->tpms_edit_id_bytes,
            sizeof(subghz->tpms_edit_id_bytes));
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdByteInput);
        return true;
    }
    if(event.event == RowTransmit) {
        int proto =
            (int)scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneTpmsSpoofConfig);
        if(tpms_spoof_seed(
               subghz,
               (size_t)proto,
               cfg_id(subghz),
               cfg_pressure(),
               (float)cfg_temp(),
               k_freqs[s_freq_idx])) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        } else {
            furi_string_set(subghz->error_str, "TPMS spoof seed failed.\nTry 433.92 MHz.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        }
        return true;
    }
    return false; // OK on a value-picker row: no-op (value changes via left/right)
}

void subghz_scene_tpms_spoof_config_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_reset(subghz->variable_item_list);
}

#endif // PUBLIC_BUILD
