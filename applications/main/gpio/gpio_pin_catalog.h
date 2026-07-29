#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pin capability flags */
typedef enum {
    GpioCapOutput = 1 << 0,
    GpioCapInput  = 1 << 1,
    GpioCapPwm    = 1 << 2,
    GpioCapAdc    = 1 << 3,
} GpioCap;

/* Pin tier classification */
typedef enum {
    GpioTierSafe,
    GpioTierAdvanced,
} GpioTier;

/* Catalog entry for a GPIO pin */
typedef struct {
    const char* name;   // e.g. "GPIO2 (Grove SDA)"
    uint8_t     gpio;   // ESP32-S3 GPIO number
    int8_t      adc_ch; // ADC1 channel, or -1 if not ADC-capable
    GpioTier    tier;
    uint8_t     caps;   // OR of GpioCap
    const char* warn;   // NULL, or shared-peripheral warning
} GpioCatalogEntry;

/* Count of entries visible for the given advanced state.
 * When include_advanced is false, returns only SAFE entries.
 * When true, returns all entries (SAFE + ADVANCED). */
size_t gpio_catalog_count(bool include_advanced);

/* Get the i-th visible entry (SAFE entries first, then ADVANCED when included).
 * Returns NULL for out-of-range i. */
const GpioCatalogEntry* gpio_catalog_get(size_t i, bool include_advanced);

#ifdef __cplusplus
}
#endif
