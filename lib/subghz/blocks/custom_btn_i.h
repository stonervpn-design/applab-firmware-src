#pragma once

#include "custom_btn.h"

#define PROG_MODE_OFF              (0U)
#define PROG_MODE_KEELOQ_BFT       (1U)
#define PROG_MODE_KEELOQ_APRIMATIC (2U)
#define PROG_MODE_KEELOQ_DEA_MIO   (3U)

typedef uint8_t ProgMode;

void subghz_custom_btn_set_original(uint8_t btn_code);

void subghz_custom_btn_set_max(uint8_t b);

void subghz_custom_btn_set_prog_mode(ProgMode prog_mode);

ProgMode subghz_custom_btn_get_prog_mode(void);

// --- Positional custom-button helpers (used by the Archiv WIP automotive batch:
//     fiat_v2_v3_v5, mazda_v2). Added 2026-07-24. ---

// Button-name lookup used when filling the custom-button menu labels.
typedef const char* (*subghz_custom_btn_name_fn)(uint8_t btn_code);

// Set the "original" (captured) button only if one has not been recorded yet.
void subghz_custom_btn_set_original_if_unset(uint8_t btn_code);

// Remap *btn according to the selected D-pad custom button. OK (or a NULL table)
// keeps original_btn; UP/DOWN/LEFT/RIGHT index positionally into table[0..3].
void subghz_custom_btn_apply_positional(
    uint8_t* btn,
    uint8_t original_btn,
    uint8_t custom_id,
    const uint8_t* table,
    size_t table_count);

// Populate custom-button menu labels from a protocol's button table. This fork
// keeps no per-button label store, so it is currently a no-op.
void subghz_custom_btn_fill_labels_positional(
    uint8_t current_btn,
    const uint8_t* table,
    size_t table_count,
    subghz_custom_btn_name_fn name_fn);
