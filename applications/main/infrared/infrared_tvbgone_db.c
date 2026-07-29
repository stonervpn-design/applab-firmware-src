#include "infrared_tvbgone_db.h"
#include <string.h>
#include <stdio.h>

/**
 * Embedded fallback TV-B-Gone code database.
 * Small set of the most common TV power-off codes.
 */

/* Sony TV Power Off - SIRC protocol (8-bit address, 7-bit command) */
static const uint32_t sony_power_pattern[] = {
    2400, 600,  // leader
    600, 600, 600, 600, 600, 1200,  // A0=0, A1=0, A2=0, A3=0, A4=0, A5=1
    600, 600, 600, 600,              // A6=0, A7=0, C0=0, C1=0
    600, 1200,                        // C2=1, C3=0
    600, 600, 600, 1200,              // C4=0, C5=0, C6=0, C7=1
    45000,                            // gap
};

/* LG TV Power Off - NEC protocol (simplified pattern) */
static const uint32_t lg_power_pattern[] = {
    9000, 4500,  // leader
    560, 560, 560, 560, 560, 560, 560, 560,    // bits 0-7
    560, 560, 560, 560, 560, 560, 560, 1680,   // bits 8-15 (address low)
    560, 560, 560, 560, 560, 560, 560, 560,    // bits 16-23 (address high)
    560, 1680, 560, 1680, 560, 1680, 560, 1680, // bits 24-31 (command - power off)
    560,                                         // trailer
    108000,                                      // gap
};

/* Samsung TV Power Off - NEC protocol variant */
static const uint32_t samsung_power_pattern[] = {
    9000, 4500,
    560, 1680, 560, 1680, 560, 1680, 560, 560,  // address byte 1
    560, 560, 560, 560, 560, 560, 560, 560,      // address byte 2
    560, 1680, 560, 560, 560, 560, 560, 560,     // command
    560, 1680, 560, 1680, 560, 1680, 560, 1680,  // command inverse
    560,
    108000,
};

/* Panasonic TV Power Off - Panasonic protocol */
static const uint32_t panasonic_power_pattern[] = {
    3500, 1750,  // leader
    430, 430, 430, 430, 430, 430, 430, 430,    // first byte
    430, 430, 430, 430, 430, 1320, 430, 430,   // second byte (with 1 bit set)
    430, 1320, 430, 430, 430, 430, 430, 430,   // command
    430, 430, 430, 430, 430, 430, 430, 430,    // command continued
    430,
    76000,
};

/* Philips TV Power Off - RC5 protocol */
static const uint32_t philips_power_pattern[] = {
    900, 900,   // S bit
    900, 900,   // T bit
    900, 2700,  // bit 0 (address A0)
    900, 900,   // bit 1 (address A1)
    900, 900,   // bit 2 (address A2)
    900, 900,   // bit 3 (address A3)
    900, 900,   // bit 4 (address A4)
    900, 900,   // bit 5 (command C0)
    900, 2700,  // bit 6 (command C1)
    900, 2700,  // bit 7 (command C2)
    900, 900,   // bit 8 (command C3)
    900, 900,   // bit 9 (command C4)
    900, 900,   // bit 10 (command C5)
    900,
    100000,
};

/* Sharp TV Power Off - Sharp protocol */
static const uint32_t sharp_power_pattern[] = {
    4000, 4000,  // leader
    500, 500, 500, 500, 500, 500, 500, 500,    // address
    500, 500, 500, 500, 500, 500, 500, 500,    // command
    500, 1000, 500, 1000, 500, 1000, 500, 1000, // inverse command
    500, 500,
    110000,
};

/* Toshiba TV Power Off - Toshiba TC protocol */
static const uint32_t toshiba_power_pattern[] = {
    4500, 4500,  // leader
    560, 560, 560, 560, 560, 560, 560, 560,    // address
    560, 560, 560, 560, 560, 560, 560, 560,    // command
    560, 1680, 560, 560, 560, 560, 560, 560,   // command continued
    560, 560, 560, 1680,
    80000,
};

/* Embedded database of common TV power codes */
static const IrUniversalCode embedded_codes[] = {
    {
        .brand = "Sony",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = sony_power_pattern,
        .pattern_len = sizeof(sony_power_pattern) / sizeof(sony_power_pattern[0]),
        .frequency = 40000,
    },
    {
        .brand = "LG",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = lg_power_pattern,
        .pattern_len = sizeof(lg_power_pattern) / sizeof(lg_power_pattern[0]),
        .frequency = 38000,
    },
    {
        .brand = "Samsung",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = samsung_power_pattern,
        .pattern_len = sizeof(samsung_power_pattern) / sizeof(samsung_power_pattern[0]),
        .frequency = 38000,
    },
    {
        .brand = "Panasonic",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = panasonic_power_pattern,
        .pattern_len = sizeof(panasonic_power_pattern) / sizeof(panasonic_power_pattern[0]),
        .frequency = 36000,
    },
    {
        .brand = "Philips",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = philips_power_pattern,
        .pattern_len = sizeof(philips_power_pattern) / sizeof(philips_power_pattern[0]),
        .frequency = 36000,
    },
    {
        .brand = "Sharp",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = sharp_power_pattern,
        .pattern_len = sizeof(sharp_power_pattern) / sizeof(sharp_power_pattern[0]),
        .frequency = 38000,
    },
    {
        .brand = "Toshiba",
        .model = "Generic TV",
        .code_name = "Power Off",
        .pattern = toshiba_power_pattern,
        .pattern_len = sizeof(toshiba_power_pattern) / sizeof(toshiba_power_pattern[0]),
        .frequency = 38000,
    },
};

#define EMBEDDED_CODE_COUNT (sizeof(embedded_codes) / sizeof(embedded_codes[0]))

/* Module state */
static const IrUniversalCode* active_db = embedded_codes;
static uint16_t active_db_count = EMBEDDED_CODE_COUNT;
static uint16_t iterator_index = 0;

/* Brands cache (lazy-built) */
static const char* brands_cache[EMBEDDED_CODE_COUNT];
static uint16_t brands_cache_count = 0;
static bool brands_cache_valid = false;

/**
 * Load TV-B-Gone database from SD or use embedded fallback.
 * Stub implementation: always uses embedded fallback.
 * In a real implementation, this would parse /ext/infrared/tvbgone_db.json.
 */
uint16_t infrared_tvbgone_load_db(void) {
    // Reset state
    active_db = embedded_codes;
    active_db_count = EMBEDDED_CODE_COUNT;
    iterator_index = 0;
    brands_cache_valid = false;

    // TODO: Attempt to load from /ext/infrared/tvbgone_db.json here
    // For now, just use embedded codes.

    return active_db_count;
}

/**
 * Get a code by index.
 */
const IrUniversalCode* infrared_tvbgone_get_code(uint16_t idx) {
    if (idx >= active_db_count) {
        return NULL;
    }
    return &active_db[idx];
}

/**
 * Get total count of loaded codes.
 */
uint16_t infrared_tvbgone_get_code_count(void) {
    return active_db_count;
}

/**
 * Build and cache the unique brands list.
 */
static void build_brands_cache(void) {
    if (brands_cache_valid) {
        return;
    }

    brands_cache_count = 0;
    for (uint16_t i = 0; i < active_db_count; i++) {
        const char* brand = active_db[i].brand;
        bool found = false;

        // Check if brand already in cache
        for (uint16_t j = 0; j < brands_cache_count; j++) {
            if (strcmp(brands_cache[j], brand) == 0) {
                found = true;
                break;
            }
        }

        // Add to cache if not found
        if (!found && brands_cache_count < EMBEDDED_CODE_COUNT) {
            brands_cache[brands_cache_count] = brand;
            brands_cache_count++;
        }
    }

    brands_cache_valid = true;
}

/**
 * Get list of unique brands.
 */
const char** infrared_tvbgone_brands(uint16_t* out_count) {
    if (!out_count) {
        return NULL;
    }

    build_brands_cache();
    *out_count = brands_cache_count;
    return (const char**)brands_cache;
}

/**
 * Reset iterator.
 */
void infrared_tvbgone_iter_reset(void) {
    iterator_index = 0;
}

/**
 * Get next code from iterator.
 */
const IrUniversalCode* infrared_tvbgone_iter_next(void) {
    if (iterator_index >= active_db_count) {
        return NULL;
    }
    return &active_db[iterator_index++];
}
