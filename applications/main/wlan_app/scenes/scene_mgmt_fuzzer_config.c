#include "../wlan_app.h"

// PRIVATE-only: 802.11 Mgmt Fuzzer frame-type + mutation-mode picker (WF1
// Task 3). Mirrors subghz_scene_protocol_fuzzer_config.c's VariableItemList
// shape (attack_targets.c / handshake_settings.c in this app use the same
// idiom): a couple of cycled items + a trailing "Run" item that hands off to
// the run scene. No ADD_SCENE reference to this file exists in a
// PUBLIC_BUILD compile (scene_config.h gates it), so this whole body is also
// guarded to add 0 symbols there.
#ifndef PUBLIC_BUILD

#include "../helpers/wlan_mgmt_fuzz.h"

// Frame-type index -- kept in sync by hand with the switch in
// scene_mgmt_fuzzer_run.c (no shared header; this file is the only writer).
enum MgmtFuzzerFrameType {
    MgmtFuzzerFrameAuth = 0,
    MgmtFuzzerFrameAssoc,
    MgmtFuzzerFrameProbe,
    MgmtFuzzerFrameBeacon,
    MgmtFuzzerFrameDeauth,
    MgmtFuzzerFrameTypeCount,
};

// Selections, read by scene_mgmt_fuzzer_run.c via extern (no WlanApp field
// added -- keeps this feature's footprint to the 4 files the task touches).
uint8_t wlan_mgmt_fuzzer_frame_type = MgmtFuzzerFrameAuth;
uint8_t wlan_mgmt_fuzzer_mode = WMF_BITFLIP;

static const char* const frame_type_names[MgmtFuzzerFrameTypeCount] = {
    "Auth",
    "Assoc",
    "Probe",
    "Beacon",
    "Deauth",
};

static const char* const fuzz_mode_names[WMF_MODE_COUNT] = {
    "BitFlip",
    "Field",
    "IE Mangle",
    "LenSweep",
    "Random",
};

enum MgmtFuzzerConfigIndex {
    MgmtFuzzerConfigIndexFrameType,
    MgmtFuzzerConfigIndexMode,
    MgmtFuzzerConfigIndexRun,
};

static void mgmt_fuzzer_config_set_frame_type(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, frame_type_names[index]);
    wlan_mgmt_fuzzer_frame_type = index;
}

static void mgmt_fuzzer_config_set_mode(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, fuzz_mode_names[index]);
    wlan_mgmt_fuzzer_mode = index;
}

static void mgmt_fuzzer_config_enter_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    if(index == MgmtFuzzerConfigIndexRun) {
        scene_manager_next_scene(app->scene_manager, WlanAppSceneMgmtFuzzerRun);
    }
}

void wlan_app_scene_mgmt_fuzzer_config_on_enter(void* context) {
    WlanApp* app = context;
    VariableItemList* list = app->variable_item_list;
    variable_item_list_reset(list);

    if(wlan_mgmt_fuzzer_frame_type >= MgmtFuzzerFrameTypeCount) wlan_mgmt_fuzzer_frame_type = 0;
    if(wlan_mgmt_fuzzer_mode >= WMF_MODE_COUNT) wlan_mgmt_fuzzer_mode = 0;

    VariableItem* item;

    item = variable_item_list_add(
        list, "Frame Type", MgmtFuzzerFrameTypeCount, mgmt_fuzzer_config_set_frame_type, app);
    variable_item_set_current_value_index(item, wlan_mgmt_fuzzer_frame_type);
    variable_item_set_current_value_text(item, frame_type_names[wlan_mgmt_fuzzer_frame_type]);

    item = variable_item_list_add(list, "Mode", WMF_MODE_COUNT, mgmt_fuzzer_config_set_mode, app);
    variable_item_set_current_value_index(item, wlan_mgmt_fuzzer_mode);
    variable_item_set_current_value_text(item, fuzz_mode_names[wlan_mgmt_fuzzer_mode]);

    variable_item_list_add(list, "Run", 1, NULL, NULL);

    variable_item_list_set_enter_callback(list, mgmt_fuzzer_config_enter_cb, app);
    variable_item_list_set_selected_item(list, 0);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_mgmt_fuzzer_config_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wlan_app_scene_mgmt_fuzzer_config_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}

#endif // PUBLIC_BUILD
