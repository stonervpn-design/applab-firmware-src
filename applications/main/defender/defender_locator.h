#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct DefenderLocator DefenderLocator;

/**
 * Allocate a new locator controller.
 * Defaults: floor=-90, ceil=-40, label="", rssi=0, stale=true
 */
DefenderLocator* defender_locator_alloc(void);

/**
 * Free a locator controller.
 */
void defender_locator_free(DefenderLocator* locator);

/**
 * Set RSSI bounds for bar/beep mapping.
 */
void defender_locator_set_bounds(DefenderLocator* locator, int8_t rssi_floor, int8_t rssi_ceil);

/**
 * Set the label (e.g., "Target Device Name").
 */
void defender_locator_set_label(DefenderLocator* locator, const char* label);

/**
 * Push a fresh RSSI sample with stale flag.
 * Applies EMA smoothing internally.
 */
void defender_locator_push(DefenderLocator* locator, int8_t rssi, bool stale);

/**
 * Get the internal View for dispatcher attachment.
 */
View* defender_locator_get_view(DefenderLocator* locator);
