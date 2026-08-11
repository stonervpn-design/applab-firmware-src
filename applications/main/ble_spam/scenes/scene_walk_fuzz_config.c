#include "../ble_spam_app.h"

// PRIVATE-only, same reasoning as scene_walk_target_info.c: the fuzz_* app
// fields and the ADD_SCENE registration referencing these functions only
// exist under #ifndef PUBLIC_BUILD -- this file must compile to an empty
// translation unit on a PUBLIC_BUILD compile.
#ifndef PUBLIC_BUILD

#include <lib/toolbox/value_index.h>
#include <string.h>

#define TAG "BleWalkFuzzConfig"

// VariableItemList config scene for the BLE GATT Fuzzer/DoS (PRIVATE-only).
// Fills app->fuzz_mode / app->fuzz_cfg / app->fuzz_write_norsp /
// app->fuzz_rate_ms / app->fuzz_count (the params ble_walk_fuzz_config()
// takes) and hands off to WalkFuzzRun on "Run". Mirrors the Sub-GHz Protocol
// Fuzzer's config scene (subghz_scene_protocol_fuzzer_config.c), trimmed to
// the params this task's brief calls out: mode + strategy + write-type +
// rate + count (no protocol/frequency/preset/seed-file -- there's no radio
// frequency to pick and the "seed" is just the fuzzer_mutate byte buffer, not
// user-facing here).
//
// app->fuzz_mode arrives pre-set by the caller scene (WalkCharDetail ->
// Sweep, WalkTargetInfo's Arm DoS -> Dos) and is just the Mode item's initial
// selection -- still user-editable here, same as every other item.

enum FuzzConfigIndex {
    FuzzConfigIndexMode,
    FuzzConfigIndexStrategy,
    FuzzConfigIndexWriteType,
    FuzzConfigIndexRate,
    FuzzConfigIndexCount,
    FuzzConfigIndexRun,
};

// Order matches BleFuzzMode (ble_walk_fuzz.h) so index casts directly.
static const char* const mode_names[] = {"Sweep", "Stack", "DoS"};
#define MODE_COUNT COUNT_OF(mode_names)

// Order matches FuzzStrategy (fuzzer_mutate.h) so index casts directly.
// Only meaningful for Sweep/Stack -- ignored by DoS (ble_walk_fuzz.h).
static const char* const strategy_names[] = {"BitFlip", "Boundary", "Length", "Random", "FieldWalk"};
#define STRATEGY_COUNT COUNT_OF(strategy_names)

static const char* const write_type_names[] = {"WithResp", "NoResp"};
#define WRITE_TYPE_COUNT COUNT_OF(write_type_names)

// Sweep/Stack: inter-iteration delay. DoS: reinterpreted as per-connection
// dwell (ble_walk_fuzz_config()'s rate_ms doc) -- 0 there means max churn.
static const uint32_t rate_values[] = {0, 10, 20, 50, 100, 200, 500, 1000};
static const char* const rate_names[] = {"0(max)", "10", "20", "50", "100", "200", "500", "1000"};
#define RATE_COUNT COUNT_OF(rate_values)
#define RATE_DEFAULT_INDEX 3 // 50ms

// Sweep/Stack: optional frame-count cap (0 = full sweep). DoS: optional
// connect-cycle cap (0 = unbounded, run until Stop).
static const uint32_t count_values[] = {0, 10, 50, 100, 500, 1000, 5000};
static const char* const count_names[] = {"Full", "10", "50", "100", "500", "1000", "5000"};
#define COUNT_COUNT COUNT_OF(count_values)

// Strategy-specific mutation parameters not surfaced as their own list items
// (same scope note as fuzzer_config_apply_strategy_defaults in the Sub-GHz
// fuzzer's config scene): sane fixed defaults applied when a strategy is
// selected.
static void fuzz_config_apply_strategy_defaults(FuzzConfig* cfg, FuzzStrategy strategy) {
    switch(strategy) {
    case FuzzStrategyLength:
        cfg->len_min_delta = -4;
        cfg->len_max_delta = 4;
        break;
    case FuzzStrategyRandom:
        cfg->rand_k = 3;
        cfg->rand_iters = 64;
        break;
    case FuzzStrategyFieldWalk:
        cfg->field_off = 0;
        cfg->field_len = 8; // walk a byte -> 256 frames
        break;
    default:
        break;
    }
}

static void fuzz_config_set_mode(VariableItem* item) {
    BleSpamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, mode_names[index]);
    app->fuzz_mode = (BleFuzzMode)index;
}

static void fuzz_config_set_strategy(VariableItem* item) {
    BleSpamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, strategy_names[index]);
    app->fuzz_cfg.strategy = (FuzzStrategy)index;
    fuzz_config_apply_strategy_defaults(&app->fuzz_cfg, (FuzzStrategy)index);
}

static void fuzz_config_set_write_type(VariableItem* item) {
    BleSpamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, write_type_names[index]);
    app->fuzz_write_norsp = (index == 1);
}

static void fuzz_config_set_rate(VariableItem* item) {
    BleSpamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, rate_names[index]);
    app->fuzz_rate_ms = rate_values[index];
}

static void fuzz_config_set_count(VariableItem* item) {
    BleSpamApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, count_names[index]);
    app->fuzz_count = count_values[index];
}

static void fuzz_config_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    BleSpamApp* app = context;
    if(index == FuzzConfigIndexRun) {
        scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkFuzzRun);
    }
}

void ble_spam_scene_walk_fuzz_config_on_enter(void* context) {
    BleSpamApp* app = context;
    VariableItem* item;
    uint8_t value_index;

    // First entry (or after ble_spam_app_alloc's memset): seed sane defaults.
    // app->fuzz_mode/app->fuzz_char_handle/app->fuzz_target arrive already set
    // by the caller scene (WalkCharDetail or WalkTargetInfo) and are left
    // alone here.
    if(app->fuzz_cfg.seed_nbits == 0) {
        app->fuzz_cfg.mode = FuzzModeProtocol; // byte-payload seed, not RAW durations
        app->fuzz_cfg.strategy = FuzzStrategyBitFlipSweep;
        app->fuzz_cfg.seed = 0xA5A5A5A5UL;
        app->fuzz_cfg.seed_nbits = 24;
        memset(app->fuzz_cfg.seed_bits, 0, sizeof(app->fuzz_cfg.seed_bits));
        app->fuzz_cfg.seed_bits[0] = 0x12;
        app->fuzz_cfg.seed_bits[1] = 0x34;
        app->fuzz_cfg.seed_bits[2] = 0x56;
        app->fuzz_write_norsp = false;
        app->fuzz_rate_ms = rate_values[RATE_DEFAULT_INDEX];
        app->fuzz_count = 0;
    }

    VariableItemList* list = app->fuzz_config_list;
    variable_item_list_reset(list);

    item = variable_item_list_add(list, "Mode", MODE_COUNT, fuzz_config_set_mode, app);
    value_index = (uint8_t)app->fuzz_mode;
    if(value_index >= MODE_COUNT) value_index = 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, mode_names[value_index]);

    item = variable_item_list_add(
        list, "Strategy", STRATEGY_COUNT, fuzz_config_set_strategy, app);
    value_index = (uint8_t)app->fuzz_cfg.strategy;
    if(value_index >= STRATEGY_COUNT) value_index = 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, strategy_names[value_index]);

    item = variable_item_list_add(
        list, "Write", WRITE_TYPE_COUNT, fuzz_config_set_write_type, app);
    value_index = app->fuzz_write_norsp ? 1 : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, write_type_names[value_index]);

    item = variable_item_list_add(list, "Rate", RATE_COUNT, fuzz_config_set_rate, app);
    value_index = (uint8_t)value_index_uint32(app->fuzz_rate_ms, rate_values, RATE_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, rate_names[value_index]);

    item = variable_item_list_add(list, "Count", COUNT_COUNT, fuzz_config_set_count, app);
    value_index = (uint8_t)value_index_uint32(app->fuzz_count, count_values, COUNT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, count_names[value_index]);

    variable_item_list_add(list, "Run", 1, NULL, NULL);

    variable_item_list_set_enter_callback(list, fuzz_config_enter_callback, app);
    variable_item_list_set_selected_item(list, 0);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWalkFuzzConfig);
}

bool ble_spam_scene_walk_fuzz_config_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void ble_spam_scene_walk_fuzz_config_on_exit(void* context) {
    BleSpamApp* app = context;
    variable_item_list_set_selected_item(app->fuzz_config_list, 0);
    variable_item_list_reset(app->fuzz_config_list);
}

#endif // PUBLIC_BUILD
