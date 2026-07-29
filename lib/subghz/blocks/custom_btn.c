#include "custom_btn_i.h"

static uint8_t custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
static uint8_t custom_btn_original = 0;
static uint8_t custom_btn_max_btns = 0;
static uint8_t controller_programming_mode = PROG_MODE_OFF;

bool subghz_custom_btn_set(uint8_t btn_id) {
    if(btn_id > custom_btn_max_btns) {
        custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
        return false;
    } else {
        custom_btn_id = btn_id;
        return true;
    }
}

uint8_t subghz_custom_btn_get(void) {
    return custom_btn_id;
}

void subghz_custom_btn_set_original(uint8_t btn_code) {
    custom_btn_original = btn_code;
}

uint8_t subghz_custom_btn_get_original(void) {
    return custom_btn_original;
}

void subghz_custom_btn_set_max(uint8_t b) {
    custom_btn_max_btns = b;
}

void subghz_custom_btns_reset(void) {
    custom_btn_original = 0;
    custom_btn_max_btns = 0;
    controller_programming_mode = PROG_MODE_OFF;
    custom_btn_id = SUBGHZ_CUSTOM_BTN_OK;
}

bool subghz_custom_btn_is_allowed(void) {
    return custom_btn_max_btns != 0;
}

void subghz_custom_btn_set_prog_mode(ProgMode prog_mode) {
    controller_programming_mode = prog_mode;
}

ProgMode subghz_custom_btn_get_prog_mode(void) {
    return controller_programming_mode;
}

void subghz_custom_btn_set_original_if_unset(uint8_t btn_code) {
    if(custom_btn_original == 0) {
        custom_btn_original = btn_code;
    }
}

void subghz_custom_btn_apply_positional(
    uint8_t* btn,
    uint8_t original_btn,
    uint8_t custom_id,
    const uint8_t* table,
    size_t table_count) {
    if(btn == NULL) return;
    // OK selection (or a missing table) keeps the captured/original button.
    if(custom_id == SUBGHZ_CUSTOM_BTN_OK || table == NULL) {
        *btn = original_btn;
        return;
    }
    // D-pad selections map positionally onto the protocol's button table:
    // UP->table[0], DOWN->table[1], LEFT->table[2], RIGHT->table[3].
    size_t idx = (size_t)(custom_id - 1);
    *btn = (idx < table_count) ? table[idx] : original_btn;
}

void subghz_custom_btn_fill_labels_positional(
    uint8_t current_btn,
    const uint8_t* table,
    size_t table_count,
    subghz_custom_btn_name_fn name_fn) {
    // This fork stores no per-button custom-menu labels (the positional label
    // store is a newer ProtoPirate UI addition absent here), so nothing to fill.
    (void)current_btn;
    (void)table;
    (void)table_count;
    (void)name_fn;
}
