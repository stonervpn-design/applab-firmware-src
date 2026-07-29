#pragma once

#include <applications/main/gpio/gpio_pin_catalog.h>

/**
 * Wiegand pin catalog — reuses GPIO catalog entries.
 * SAFE pins: Grove pins (GPIO1/GPIO2)
 * ADVANCED pins: IR TX and shared peripheral pins
 */

/**
 * Get count of available pins.
 * @param include_advanced If true, include advanced pins; otherwise only safe pins
 * @return Number of available pins
 */
size_t wiegand_catalog_count(bool include_advanced);

/**
 * Get catalog entry by index.
 * @param i Index (SAFE entries first, then ADVANCED)
 * @param include_advanced If true, include advanced pins; otherwise only safe pins
 * @return Pointer to catalog entry, or NULL if out of range
 */
const GpioCatalogEntry* wiegand_catalog_get(size_t i, bool include_advanced);
