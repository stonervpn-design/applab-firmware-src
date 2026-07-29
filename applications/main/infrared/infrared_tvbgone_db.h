#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * TV-B-Gone universal IR code database core.
 * Pure C, no dependencies beyond stdlib.
 * Loads from SD (JSON) or uses embedded fallback set.
 */

typedef struct {
    const char* brand;        // "Sony", "LG", etc.
    const char* model;        // "Generic TV", "Projector", etc.
    const char* code_name;    // "Power Off", "Mute", etc.
    const uint32_t* pattern;  // Raw IR pattern (durations in µs)
    uint16_t pattern_len;     // Number of timings in pattern
    uint32_t frequency;       // 38000 Hz typical
} IrUniversalCode;

/**
 * Load TV-B-Gone database from SD card (/ext/infrared/tvbgone_db.json)
 * or fall back to embedded codes if SD load fails.
 * Returns count of codes available.
 */
uint16_t infrared_tvbgone_load_db(void);

/**
 * Get a code by index (0-based).
 * Returns NULL if index is out of range.
 */
const IrUniversalCode* infrared_tvbgone_get_code(uint16_t idx);

/**
 * Get total count of loaded codes.
 */
uint16_t infrared_tvbgone_get_code_count(void);

/**
 * Get list of unique brands in the database.
 * out_count: pointer to receive the count of brands.
 * Returns pointer to array of brand name strings (owned by this module).
 */
const char** infrared_tvbgone_brands(uint16_t* out_count);

/**
 * Reset iterator to start (for next fetch sequence).
 */
void infrared_tvbgone_iter_reset(void);

/**
 * Get next code from the iterator.
 * Returns pointer to code struct, or NULL when iteration is complete.
 * Call infrared_tvbgone_iter_reset() to restart.
 */
const IrUniversalCode* infrared_tvbgone_iter_next(void);
