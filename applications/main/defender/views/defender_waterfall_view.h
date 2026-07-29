#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

#define WATERFALL_MAXBINS 64
#define WATERFALL_HISTW   64

/**
 * Waterfall view: scrolling RSSI intensity history.
 * Pure display, no radio knowledge. Reusable for Sub-GHz, nRF24, etc.
 */

typedef struct {
    uint8_t col[WATERFALL_MAXBINS];      // Current column intensities (0-255)
    uint8_t nbins;                        // Number of valid bins in col[0..nbins-1]
    int8_t peak_bin;                      // Index of peak bin (-1 if none), for labeling
    char label[24];                       // Description (e.g., "433.92 MHz")
    uint8_t hist[WATERFALL_HISTW][WATERFALL_MAXBINS]; // Scrolling history
    uint8_t hist_head;                    // Write pointer (oldest column)
} DefenderWaterfallModel;

/**
 * Allocate a waterfall view.
 */
View* defender_waterfall_view_alloc(void);

/**
 * Free a waterfall view.
 */
void defender_waterfall_view_free(View* view);

/**
 * Push a new column of intensities; scrolls left.
 *
 * @param view       Waterfall view
 * @param col        Array of intensities (0-255); NULL clears to 0
 * @param nbins      Number of valid bins
 * @param peak_bin   Index of peak (-1 if none)
 */
void defender_waterfall_view_push(
    View* view,
    const uint8_t* col,
    uint8_t nbins,
    int8_t peak_bin);

/**
 * Set the label (frequency, mode, etc.).
 */
void defender_waterfall_view_set_label(View* view, const char* label);
