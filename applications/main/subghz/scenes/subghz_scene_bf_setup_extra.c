#include "../subghz_i.h"
#include "../helpers/subbrute_custom_event.h"

#define TAG "SubGhzSceneBfSetupExtra"

#define MIN_TD  0
#define MAX_TD  255
#define MIN_REP 1
#define MAX_REP 100
#define MIN_TE  100
#define MAX_TE  600

enum SubBruteVarListIndex {
    SubBruteVarListIndexTimeDelay,
    SubBruteVarListIndexRepeatOrOnExtra,
    SubBruteVarListIndexTe,
    SubBruteVarListIndexOpenCode,
};

static void setup_extra_enter_callback(void* context, uint32_t index);

static void setup_extra_td_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    char buf[6] = {0};

    const uint8_t index = variable_item_get_current_value_index(item);
    uint8_t val = subbrute_worker_get_timeout(subghz->subbrute_worker);

    if(index == 0) {
        if(val > MIN_TD) {
            val--;
            subbrute_worker_set_timeout(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_TD) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    } else if(index == 2) {
        if(val < MAX_TD) {
            val++;
            subbrute_worker_set_timeout(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_TD) {
                variable_item_set_current_value_index(item, 2);
            }
        }
    } else if(index == 1) {
        if(val == MIN_TD) {
            val++;
            subbrute_worker_set_timeout(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_TD) {
                variable_item_set_current_value_index(item, 2);
            }
        } else if(val == MAX_TD) {
            val--;
            subbrute_worker_set_timeout(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_TD) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    }
}

static void setup_extra_rep_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    char buf[6] = {0};

    const uint8_t index = variable_item_get_current_value_index(item);
    uint8_t val = subbrute_worker_get_repeats(subghz->subbrute_worker);

    if(index == 0) {
        if(val > MIN_REP) {
            val--;
            subbrute_worker_set_repeats(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_REP) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    } else if(index == 2) {
        if(val < MAX_REP) {
            val++;
            subbrute_worker_set_repeats(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_REP) {
                variable_item_set_current_value_index(item, 2);
            }
        }
    } else if(index == 1) {
        if(val == MIN_REP) {
            val++;
            subbrute_worker_set_repeats(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_REP) {
                variable_item_set_current_value_index(item, 2);
            }
        } else if(val == MAX_REP) {
            val--;
            subbrute_worker_set_repeats(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%d", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_REP) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    }
}

static void setup_extra_te_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    char buf[6] = {0};

    const uint8_t index = variable_item_get_current_value_index(item);
    uint32_t val = subbrute_worker_get_te(subghz->subbrute_worker);

    if(index == 0) {
        if(val > MIN_TE) {
            val--;
            subbrute_worker_set_te(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%ld", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_TE) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    } else if(index == 2) {
        if(val < MAX_TE) {
            val++;
            subbrute_worker_set_te(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%ld", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_TE) {
                variable_item_set_current_value_index(item, 2);
            }
        }
    } else if(index == 1) {
        if(val == MIN_TE) {
            val++;
            subbrute_worker_set_te(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%ld", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MAX_TE) {
                variable_item_set_current_value_index(item, 2);
            }
        } else if(val == MAX_TE) {
            val--;
            subbrute_worker_set_te(subghz->subbrute_worker, val);
            snprintf(&buf[0], 5, "%ld", val);
            variable_item_set_current_value_text(item, &buf[0]);
            variable_item_set_current_value_index(item, 1);
            if(val == MIN_TE) {
                variable_item_set_current_value_index(item, 0);
            }
        }
    }
}

const char* const opencode_names[] =
    {"0001", "0010", "0100", "1000", "1100", "0F00", "00F0", "F000", "1001"};
const uint8_t opencode_values[COUNT_OF(opencode_names)] = {0, 1, 2, 3, 4, 5, 6, 7, 8};

static void setup_extra_opencode_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    const uint8_t value_index = variable_item_get_current_value_index(item);
    subbrute_worker_set_opencode(subghz->subbrute_worker, opencode_values[value_index]);
    subghz->subbrute_device->opencode = opencode_values[value_index];
    variable_item_set_current_value_text(item, opencode_names[value_index]);
}

#ifndef PUBLIC_BUILD
static void setup_extra_fast_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    const uint8_t value_index = variable_item_get_current_value_index(item);
    subbrute_worker_set_fast(subghz->subbrute_worker, value_index == 1);
    variable_item_set_current_value_text(item, value_index == 1 ? "On" : "Off");
}

// Function-mask brute: enumerate only the low N button/data bits of the captured
// key (2^N codes) instead of the byte-granular sweep. One control selects Off or
// the width; the label shows N + the resulting code count. See
// docs/superpowers/specs/2026-08-09-function-mask-brute-design.md.
static const char* const mask_names[] = {"Off", "2b/4", "4b/16", "8b/256", "12b/4k"};
static const uint8_t mask_nbits[] = {0, 2, 4, 8, 12};

static void setup_extra_mask_callback(VariableItem* item) {
    furi_assert(item);
    SubGhz* subghz = variable_item_get_context(item);
    furi_assert(subghz);
    const uint8_t vi = variable_item_get_current_value_index(item);
    const uint8_t nbits = mask_nbits[vi];
    const bool on = nbits > 0;
    const uint64_t var = on ? (((uint64_t)1 << nbits) - 1u) : 0;
    subbrute_worker_set_func_mask(subghz->subbrute_worker, var, on);
    subbrute_worker_apply_func_mask(subghz->subbrute_worker); // recompute 2^N + reset step
    subghz->subbrute_device->current_step = 0; // fresh masked sweep
    variable_item_set_current_value_text(item, mask_names[vi]);
}
#endif

static void subghz_scene_bf_setup_extra_init_var_list(SubGhz* subghz, bool on_extra) {
    furi_assert(subghz);
    char str[6] = {0};
    VariableItem* item;
    static bool extra = false;
    if(on_extra) {
        extra = true;
    }

    VariableItemList* var_list = subghz->variable_item_list;

    variable_item_list_reset(var_list);

    item = variable_item_list_add(var_list, "TimeDelay", 3, setup_extra_td_callback, subghz);
    snprintf(&str[0], 5, "%d", subbrute_worker_get_timeout(subghz->subbrute_worker));
    variable_item_set_current_value_text(item, &str[0]);
    switch(subbrute_worker_get_timeout(subghz->subbrute_worker)) {
    case MIN_TD:
        variable_item_set_current_value_index(item, 0);
        break;
    case MAX_TD:
        variable_item_set_current_value_index(item, 2);
        break;

    default:
        variable_item_set_current_value_index(item, 1);
        break;
    }

    if(extra) {
        item = variable_item_list_add(var_list, "Repeats", 3, setup_extra_rep_callback, subghz);
        snprintf(&str[0], 5, "%d", subbrute_worker_get_repeats(subghz->subbrute_worker));
        variable_item_set_current_value_text(item, &str[0]);
        switch(subbrute_worker_get_repeats(subghz->subbrute_worker)) {
        case MIN_REP:
            variable_item_set_current_value_index(item, 0);
            break;
        case MAX_REP:
            variable_item_set_current_value_index(item, 2);
            break;

        default:
            variable_item_set_current_value_index(item, 1);
            break;
        }
        const uint32_t te = subbrute_worker_get_te(subghz->subbrute_worker);
        if(te != 0) {
            item = variable_item_list_add(var_list, "Te", 3, setup_extra_te_callback, subghz);
            snprintf(&str[0], 5, "%ld", te);
            variable_item_set_current_value_text(item, &str[0]);
            switch(te) {
            case MIN_TE:
                variable_item_set_current_value_index(item, 0);
                break;
            case MAX_TE:
                variable_item_set_current_value_index(item, 2);
                break;

            default:
                variable_item_set_current_value_index(item, 1);
                break;
            }
        }

        if(subbrute_worker_get_is_pt2262(subghz->subbrute_worker)) {
            uint8_t value_index;
            item = variable_item_list_add(
                var_list, "PT2262Code", 9, setup_extra_opencode_callback, subghz);
            value_index = subbrute_worker_get_opencode(subghz->subbrute_worker);
            variable_item_set_current_value_index(item, value_index);
            variable_item_set_current_value_text(item, opencode_names[value_index]);
        }

#ifndef PUBLIC_BUILD
        item = variable_item_list_add(var_list, "Fast", 2, setup_extra_fast_callback, subghz);
        {
            const uint8_t fi = subbrute_worker_is_fast(subghz->subbrute_worker) ? 1 : 0;
            variable_item_set_current_value_index(item, fi);
            variable_item_set_current_value_text(item, fi == 1 ? "On" : "Off");
        }

        // Function-mask brute (LoadFile only) — enumerate just the low N bits.
        item = variable_item_list_add(
            var_list, "Mask", COUNT_OF(mask_names), setup_extra_mask_callback, subghz);
        {
            const uint8_t mb = subbrute_worker_get_mask_bits(subghz->subbrute_worker);
            uint8_t mi = 0;
            for(uint8_t k = 0; k < COUNT_OF(mask_nbits); k++) {
                if(mask_nbits[k] == mb) {
                    mi = k;
                    break;
                }
            }
            variable_item_set_current_value_index(item, mi);
            variable_item_set_current_value_text(item, mask_names[mi]);
        }
#endif
    } else {
        item = variable_item_list_add(var_list, "Show Extra", 0, NULL, NULL);
        variable_item_set_current_value_index(item, 0);
    }

    variable_item_list_set_selected_item(var_list, 0);

    variable_item_list_set_enter_callback(var_list, setup_extra_enter_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

static void setup_extra_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;

    if(index == SubBruteVarListIndexRepeatOrOnExtra) {
        subghz_scene_bf_setup_extra_init_var_list(subghz, true);
    }
}

void subghz_scene_bf_setup_extra_on_enter(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;

    subghz_scene_bf_setup_extra_init_var_list(subghz, false);
}

void subghz_scene_bf_setup_extra_on_exit(void* context) {
    furi_assert(context);
    SubGhz* subghz = (SubGhz*)context;

    variable_item_list_reset(subghz->variable_item_list);
}

bool subghz_scene_bf_setup_extra_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}
