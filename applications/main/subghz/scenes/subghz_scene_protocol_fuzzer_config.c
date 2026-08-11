#include "../subghz_i.h"

// This whole scene body is PRIVATE-only: subghz_i.h only declares the
// FuzzConfig/FuzzerWorker/SubGhzViewFuzzer types (and the fuzzer_cfg/
// fuzzer_protocol/... struct fields this file reads/writes) under
// #ifndef PUBLIC_BUILD, and the ADD_SCENE registration that would reference
// these functions is guarded the same way (subghz_scene_config.h) — so on a
// PUBLIC_BUILD compile this file must compile to an empty translation unit
// rather than fail on the now-missing types/fields.
#ifndef PUBLIC_BUILD

#include <lib/toolbox/value_index.h>
#include <storage/storage.h>
#include <flipper_format.h>
#include <flipper_format_i.h>
#include <string.h>

#define TAG "SubGhzSceneProtocolFuzzerConfig"

// VariableItemList config scene for the Protocol Fuzzer (PRIVATE-only). Fills
// subghz->fuzzer_cfg (+ fuzzer_protocol/fuzzer_freq/fuzzer_preset/fuzzer_te/
// fuzzer_repeat — the params fuzzer_worker_set_config() takes beyond
// FuzzConfig itself) and hands off to the run scene on "Run".

enum FuzzerConfigIndex {
    FuzzerConfigIndexMode,
    FuzzerConfigIndexProtocol,
    FuzzerConfigIndexFrequency,
    FuzzerConfigIndexPreset,
    FuzzerConfigIndexSeed,
    FuzzerConfigIndexStrategy,
    FuzzerConfigIndexRate,
    FuzzerConfigIndexRepeat,
    FuzzerConfigIndexRun,
};

static const char* const mode_names[] = {"RAW", "Protocol"};
#define MODE_COUNT COUNT_OF(mode_names)

// Registry names from lib/subghz/protocols/*.h (SUBGHZ_PROTOCOL_*_NAME) — the
// exact strings subghz_transmitter_alloc_init() looks up.
static const char* const protocol_names[] = {"Princeton", "CAME", "Nice FLO", "Holtek", "Linear"};
#define PROTOCOL_COUNT COUNT_OF(protocol_names)

static const uint32_t freq_values[] =
    {300000000, 315000000, 390000000, 433920000, 868350000, 915000000};
static const char* const freq_names[] = {"300.00", "315.00", "390.00", "433.92", "868.35", "915.00"};
#define FREQ_COUNT COUNT_OF(freq_values)
#define FREQ_DEFAULT_INDEX 3 // 433.92 MHz

static const char* const preset_names[] = {"OOK650", "OOK270", "FM238"};
// FuriHalSubGhzPreset enum names, matching subbrute_protocol_convert_preset()'s
// expected input (see fuzzer_worker.h's preset doc).
static const char* const preset_values[] = {
    "FuriHalSubGhzPresetOok650Async",
    "FuriHalSubGhzPresetOok270Async",
    "FuriHalSubGhzPreset2FSKDev238Async",
};
#define PRESET_COUNT COUNT_OF(preset_names)

static const char* const seed_names[] = {"Default", "Load"};
#define SEED_COUNT COUNT_OF(seed_names)

// Order matches FuzzStrategy (fuzzer_mutate.h) so index casts directly.
static const char* const strategy_names[] = {"BitFlip", "Boundary", "Length", "Random", "FieldWalk"};
#define STRATEGY_COUNT COUNT_OF(strategy_names)

// "Rate" = TE (protocol timing element, us) — the only per-sweep timing knob
// fuzzer_worker_set_config() exposes.
static const uint32_t rate_values[] = {100, 150, 200, 250, 300, 350, 400, 450, 500, 600};
static const char* const rate_names[] =
    {"100", "150", "200", "250", "300", "350", "400", "450", "500", "600"};
#define RATE_COUNT COUNT_OF(rate_values)
#define RATE_DEFAULT_INDEX 6 // 400us

#define REPEAT_MIN 1
#define REPEAT_MAX 20
#define REPEAT_DEFAULT 10

// Strategy-specific mutation parameters not surfaced as their own list items
// (task-7 scope): sane fixed defaults applied when a strategy is selected.
static void fuzzer_config_apply_strategy_defaults(FuzzConfig* cfg, FuzzStrategy strategy) {
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

// Placeholder seed so the sweep can run without a real capture loaded (Seed:
// Load replaces this with actual frame bytes/durations from a .sub file).
static void fuzzer_config_apply_default_seed(FuzzConfig* cfg) {
    if(cfg->mode == FuzzModeProtocol) {
        cfg->seed_nbits = 24;
        memset(cfg->seed_bits, 0, sizeof(cfg->seed_bits));
        cfg->seed_bits[0] = 0x12;
        cfg->seed_bits[1] = 0x34;
        cfg->seed_bits[2] = 0x56;
    } else {
        static const int16_t raw_seed[] = {
            320, -320, 320, -640, 640, -320, 320, -320, 640, -640, 320, -320, 320, -640, 320, -9000};
        memset(cfg->seed_dur, 0, sizeof(cfg->seed_dur));
        memcpy(cfg->seed_dur, raw_seed, sizeof(raw_seed));
        cfg->seed_ndur = COUNT_OF(raw_seed);
    }
}

// Load a .sub as the fuzz seed: Frequency/Preset/TE/Repeat + (Protocol: Bit+Key
// or RAW: RAW_Data) straight from the file, overriding the current config.
// Mirrors subbrute_device_load_from_file()'s field reads (subbrute_device.c).
static bool fuzzer_config_load_seed_from_file(SubGhz* subghz) {
    FuriString* app_folder = furi_string_alloc_set(EXT_PATH("subghz"));
    FuriString* load_path = furi_string_alloc();

    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, ".sub", &I_sub1_10px);

    bool picked = dialog_file_browser_show(subghz->dialogs, load_path, app_folder, &browser_options);
    bool ok = false;

    if(picked) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FlipperFormat* fff = flipper_format_file_alloc(storage);
        FuriString* temp_str = furi_string_alloc();
        uint32_t temp_u32 = 0;

        do {
            if(!flipper_format_file_open_existing(fff, furi_string_get_cstr(load_path))) {
                FURI_LOG_E(TAG, "Failed to open %s", furi_string_get_cstr(load_path));
                break;
            }
            if(!flipper_format_read_header(fff, temp_str, &temp_u32)) break;

            if(!flipper_format_read_uint32(fff, "Frequency", &temp_u32, 1)) break;
            subghz->fuzzer_freq = temp_u32;

            if(!flipper_format_read_string(fff, "Preset", temp_str)) break;
            strncpy(
                subghz->fuzzer_preset, furi_string_get_cstr(temp_str), sizeof(subghz->fuzzer_preset) - 1);
            subghz->fuzzer_preset[sizeof(subghz->fuzzer_preset) - 1] = '\0';

            if(!flipper_format_read_string(fff, "Protocol", temp_str)) break;
            if(furi_string_cmp_str(temp_str, "RAW") == 0) {
                subghz->fuzzer_cfg.mode = FuzzModeRaw;
                uint32_t count = 0;
                if(!flipper_format_get_value_count(fff, "RAW_Data", &count) || count == 0) break;
                if(count > FUZZ_MAX_DUR) count = FUZZ_MAX_DUR;
                int32_t tmp[FUZZ_MAX_DUR];
                if(!flipper_format_read_int32(fff, "RAW_Data", tmp, (uint16_t)count)) break;
                memset(subghz->fuzzer_cfg.seed_dur, 0, sizeof(subghz->fuzzer_cfg.seed_dur));
                for(uint32_t i = 0; i < count; i++) {
                    subghz->fuzzer_cfg.seed_dur[i] = (int16_t)tmp[i];
                }
                subghz->fuzzer_cfg.seed_ndur = (uint16_t)count;
            } else {
                strncpy(
                    subghz->fuzzer_protocol,
                    furi_string_get_cstr(temp_str),
                    sizeof(subghz->fuzzer_protocol) - 1);
                subghz->fuzzer_protocol[sizeof(subghz->fuzzer_protocol) - 1] = '\0';
                subghz->fuzzer_cfg.mode = FuzzModeProtocol;

                if(!flipper_format_read_uint32(fff, "Bit", &temp_u32, 1)) break;
                subghz->fuzzer_cfg.seed_nbits = (uint16_t)temp_u32;

                uint8_t key_data[sizeof(uint64_t)] = {0};
                if(!flipper_format_read_hex(fff, "Key", key_data, sizeof(key_data))) break;
                memset(subghz->fuzzer_cfg.seed_bits, 0, sizeof(subghz->fuzzer_cfg.seed_bits));
                int nb = (subghz->fuzzer_cfg.seed_nbits + 7) / 8;
                if(nb > 8) nb = 8;
                for(int i = 0; i < nb; i++) {
                    subghz->fuzzer_cfg.seed_bits[i] = key_data[8 - nb + i];
                }
            }

            if(flipper_format_read_uint32(fff, "TE", &temp_u32, 1)) {
                subghz->fuzzer_te = temp_u32;
            }
            if(flipper_format_read_uint32(fff, "Repeat", &temp_u32, 1)) {
                subghz->fuzzer_repeat = (uint8_t)temp_u32;
            }

            ok = true;
        } while(false);

        furi_string_free(temp_str);
        flipper_format_file_close(fff);
        flipper_format_free(fff);
        furi_record_close(RECORD_STORAGE);
    }

    furi_string_free(app_folder);
    furi_string_free(load_path);
    return ok;
}

static void fuzzer_config_set_mode(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, mode_names[index]);
    subghz->fuzzer_cfg.mode = (index == 1) ? FuzzModeProtocol : FuzzModeRaw;
    fuzzer_config_apply_default_seed(&subghz->fuzzer_cfg);
}

static void fuzzer_config_set_protocol(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, protocol_names[index]);
    strncpy(subghz->fuzzer_protocol, protocol_names[index], sizeof(subghz->fuzzer_protocol) - 1);
    subghz->fuzzer_protocol[sizeof(subghz->fuzzer_protocol) - 1] = '\0';
}

static void fuzzer_config_set_frequency(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, freq_names[index]);
    subghz->fuzzer_freq = freq_values[index];
}

static void fuzzer_config_set_preset(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, preset_names[index]);
    strncpy(subghz->fuzzer_preset, preset_values[index], sizeof(subghz->fuzzer_preset) - 1);
    subghz->fuzzer_preset[sizeof(subghz->fuzzer_preset) - 1] = '\0';
}

static void fuzzer_config_set_seed(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    if(index == 0) {
        fuzzer_config_apply_default_seed(&subghz->fuzzer_cfg);
        variable_item_set_current_value_text(item, seed_names[0]);
        return;
    }

    // Load: blocking file-browser + parse. On success, the load may have
    // changed mode/frequency/preset/protocol/te/repeat too — refresh those
    // items' displayed text (their stored values are already updated).
    bool ok = fuzzer_config_load_seed_from_file(subghz);
    variable_item_set_current_value_text(item, ok ? "Loaded" : "Load failed");
    if(!ok) return;

    VariableItemList* list = subghz->variable_item_list;

    VariableItem* mode_item = variable_item_list_get(list, FuzzerConfigIndexMode);
    variable_item_set_current_value_index(
        mode_item, subghz->fuzzer_cfg.mode == FuzzModeProtocol ? 1 : 0);
    variable_item_set_current_value_text(
        mode_item, mode_names[subghz->fuzzer_cfg.mode == FuzzModeProtocol ? 1 : 0]);

    VariableItem* protocol_item = variable_item_list_get(list, FuzzerConfigIndexProtocol);
    variable_item_set_current_value_text(protocol_item, subghz->fuzzer_protocol);

    VariableItem* freq_item = variable_item_list_get(list, FuzzerConfigIndexFrequency);
    {
        char buf[16];
        snprintf(
            buf,
            sizeof(buf),
            "%lu.%02lu",
            (unsigned long)(subghz->fuzzer_freq / 1000000UL),
            (unsigned long)((subghz->fuzzer_freq % 1000000UL) / 10000UL));
        variable_item_set_current_value_text(freq_item, buf);
    }

    VariableItem* preset_item = variable_item_list_get(list, FuzzerConfigIndexPreset);
    variable_item_set_current_value_text(preset_item, subghz->fuzzer_preset);

    VariableItem* rate_item = variable_item_list_get(list, FuzzerConfigIndexRate);
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)subghz->fuzzer_te);
        variable_item_set_current_value_text(rate_item, buf);
    }

    VariableItem* repeat_item = variable_item_list_get(list, FuzzerConfigIndexRepeat);
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)subghz->fuzzer_repeat);
        variable_item_set_current_value_text(repeat_item, buf);
    }
}

static void fuzzer_config_set_strategy(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, strategy_names[index]);
    subghz->fuzzer_cfg.strategy = (FuzzStrategy)index;
    fuzzer_config_apply_strategy_defaults(&subghz->fuzzer_cfg, (FuzzStrategy)index);
}

static void fuzzer_config_set_rate(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, rate_names[index]);
    subghz->fuzzer_te = rate_values[index];
}

static void fuzzer_config_set_repeat(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    char buf[6] = {0};
    const uint8_t index = variable_item_get_current_value_index(item);
    uint8_t val = subghz->fuzzer_repeat;

    if(index == 0 && val > REPEAT_MIN) {
        val--;
    } else if(index == 2 && val < REPEAT_MAX) {
        val++;
    } else if(index == 1) {
        // Snapped back to the middle position after hitting an edge — nudge
        // off the boundary so repeated presses keep working (mirrors
        // subghz_scene_bf_setup_extra.c's TimeDelay/Repeats idiom).
        if(val == REPEAT_MIN) {
            val++;
        } else if(val == REPEAT_MAX) {
            val--;
        }
    }

    subghz->fuzzer_repeat = val;
    snprintf(buf, sizeof(buf), "%u", (unsigned)val);
    variable_item_set_current_value_text(item, buf);
    variable_item_set_current_value_index(item, (val == REPEAT_MIN) ? 0 : (val == REPEAT_MAX ? 2 : 1));
}

static void fuzzer_config_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    SubGhz* subghz = context;
    if(index == FuzzerConfigIndexRun) {
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneProtocolFuzzerRun);
    }
}

void subghz_scene_protocol_fuzzer_config_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItem* item;
    uint8_t value_index;

    // First entry (or after subghz_alloc's memset): seed sane defaults.
    if(subghz->fuzzer_protocol[0] == '\0') {
        strncpy(subghz->fuzzer_protocol, protocol_names[0], sizeof(subghz->fuzzer_protocol) - 1);
        subghz->fuzzer_freq = freq_values[FREQ_DEFAULT_INDEX];
        strncpy(subghz->fuzzer_preset, preset_values[0], sizeof(subghz->fuzzer_preset) - 1);
        subghz->fuzzer_te = rate_values[RATE_DEFAULT_INDEX];
        subghz->fuzzer_repeat = REPEAT_DEFAULT;
        subghz->fuzzer_cfg.mode = FuzzModeProtocol;
        subghz->fuzzer_cfg.strategy = FuzzStrategyBitFlipSweep;
        fuzzer_config_apply_default_seed(&subghz->fuzzer_cfg);
    }

    VariableItemList* list = subghz->variable_item_list;
    variable_item_list_reset(list);

    item = variable_item_list_add(list, "Mode", MODE_COUNT, fuzzer_config_set_mode, subghz);
    value_index = (subghz->fuzzer_cfg.mode == FuzzModeProtocol) ? 1 : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, mode_names[value_index]);

    item = variable_item_list_add(
        list, "Protocol", PROTOCOL_COUNT, fuzzer_config_set_protocol, subghz);
    value_index = 0;
    for(uint8_t i = 0; i < PROTOCOL_COUNT; i++) {
        if(strcmp(protocol_names[i], subghz->fuzzer_protocol) == 0) {
            value_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, protocol_names[value_index]);

    item = variable_item_list_add(
        list, "Frequency", FREQ_COUNT, fuzzer_config_set_frequency, subghz);
    value_index = value_index_uint32(subghz->fuzzer_freq, freq_values, FREQ_COUNT);
    variable_item_set_current_value_index(item, value_index);
    {
        char buf[16];
        snprintf(
            buf,
            sizeof(buf),
            "%lu.%02lu",
            (unsigned long)(subghz->fuzzer_freq / 1000000UL),
            (unsigned long)((subghz->fuzzer_freq % 1000000UL) / 10000UL));
        variable_item_set_current_value_text(item, buf);
    }

    item = variable_item_list_add(list, "Preset", PRESET_COUNT, fuzzer_config_set_preset, subghz);
    value_index = 0;
    for(uint8_t i = 0; i < PRESET_COUNT; i++) {
        if(strcmp(preset_values[i], subghz->fuzzer_preset) == 0) {
            value_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, subghz->fuzzer_preset);

    item = variable_item_list_add(list, "Seed", SEED_COUNT, fuzzer_config_set_seed, subghz);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, seed_names[0]);

    item = variable_item_list_add(
        list, "Strategy", STRATEGY_COUNT, fuzzer_config_set_strategy, subghz);
    value_index = (uint8_t)subghz->fuzzer_cfg.strategy;
    if(value_index >= STRATEGY_COUNT) value_index = 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, strategy_names[value_index]);

    item = variable_item_list_add(list, "Rate", RATE_COUNT, fuzzer_config_set_rate, subghz);
    value_index = value_index_uint32(subghz->fuzzer_te, rate_values, RATE_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, rate_names[value_index]);

    item = variable_item_list_add(list, "Repeat", 3, fuzzer_config_set_repeat, subghz);
    {
        char buf[6];
        snprintf(buf, sizeof(buf), "%u", (unsigned)subghz->fuzzer_repeat);
        variable_item_set_current_value_text(item, buf);
        variable_item_set_current_value_index(
            item,
            (subghz->fuzzer_repeat == REPEAT_MIN) ? 0 :
                (subghz->fuzzer_repeat == REPEAT_MAX ? 2 : 1));
    }

    variable_item_list_add(list, "Run", 1, NULL, NULL);

    variable_item_list_set_enter_callback(list, fuzzer_config_enter_callback, subghz);
    variable_item_list_set_selected_item(list, 0);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_protocol_fuzzer_config_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_protocol_fuzzer_config_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_set_selected_item(subghz->variable_item_list, 0);
    variable_item_list_reset(subghz->variable_item_list);
}

#endif // PUBLIC_BUILD
