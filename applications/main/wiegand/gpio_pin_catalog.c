// VENDORED from applications/main/gpio/gpio_pin_catalog.c.
//
// wiegand_pin_catalog.c forwards to gpio_catalog_count()/gpio_catalog_get(), which
// live in the `gpio` app's translation unit. The `gpio` app is NOT built in this
// port (its gpio_items.c/usb_uart_bridge.c are unported — they need real-Flipper HAL
// symbols and usb_cdc.h), so linking it just to reach this self-contained pin-catalog
// table is impossible. Compiling this copy into the wiegand app makes wiegand link on
// its own (Cardputer). The table is identical to gpio's; keep them in sync.
//
// IF the gpio app is ever ported and added to APPS: DELETE this file and instead add
//   APP_REQUIRES = { ..., "wiegand": {"gpio"} }
// in fam_config.py, otherwise gpio_catalog_count/get would be defined twice (duplicate
// symbol link error when both apps are built).
#include <applications/main/gpio/gpio_pin_catalog.h>
#include <furi_hal_gpio.h>

#if BOARD_HAS_NRF24
/* ============================================================================
 * M5Stack Cardputer-ADV Catalog (with CC1101, NRF24, RGB)
 * ============================================================================
 *
 * SAFE: only broken-out, non-peripheral pins. On the ADV the physically
 * reachable free pins are the Grove/HY2.0 port (GPIO1/GPIO2). Both are
 * ADC1-capable (ch0/ch1). They are also QWIIC I2C / IR-RX when those are in
 * use, so they carry a soft note but remain SAFE (user opts into wiring).
 */
static const GpioCatalogEntry k_catalog[] = {
    /* --- SAFE --- */
    { "GPIO2 (Grove SDA)", 2, 1, GpioTierSafe,
      GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc, NULL },
    { "GPIO1 (Grove SCL)", 1, 0, GpioTierSafe,
      GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc,
      "also IR-RX / QWIIC SCL" },
    /* --- ADVANCED (shared with peripherals — warn) --- */
    { "GPIO44 (IR TX)",    44, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "IR transmitter pin" },
    { "GPIO13 (CC1101 CS)",13, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with CC1101 - disable Sub-GHz" },
    { "GPIO5 (CC1101 G0)",  5, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with CC1101 - disable Sub-GHz" },
    { "GPIO4 (NRF24 CE)",   4, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with NRF24" },
    { "GPIO6 (NRF24 CSN)",  6, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with NRF24" },
    { "GPIO21 (RGB LED)",  21, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with RGB LED" },
};

#else
/* ============================================================================
 * M5Stack Cardputer (Standard) Catalog
 * ============================================================================
 *
 * Standard board: no CC1101, no NRF24, no RGB LED.
 * SAFE: Grove pins only (GPIO1/GPIO2).
 * ADVANCED: IR TX (GPIO44) only.
 */
static const GpioCatalogEntry k_catalog[] = {
    /* --- SAFE --- */
    { "GPIO2 (Grove SDA)", 2, 1, GpioTierSafe,
      GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc, NULL },
    { "GPIO1 (Grove SCL)", 1, 0, GpioTierSafe,
      GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc,
      "also IR-RX / QWIIC SCL" },
    /* --- ADVANCED (shared with peripherals — warn) --- */
    { "GPIO44 (IR TX)",    44, -1, GpioTierAdvanced,
      GpioCapOutput | GpioCapInput | GpioCapPwm, "IR transmitter pin" },
};

#endif

/* ============================================================================
 * Catalog accessors
 * ============================================================================ */

#define GPIO_CATALOG_TOTAL (sizeof(k_catalog) / sizeof(k_catalog[0]))

/* Whether an entry is visible for the given advanced state.
 * SAFE entries are always visible; ADVANCED entries only when included.
 * The `tier` field is the single source of truth for the split — no
 * hardcoded counts and no assumption about array ordering. */
static bool gpio_catalog_visible(const GpioCatalogEntry* e, bool include_advanced) {
    return e->tier == GpioTierSafe || include_advanced;
}

size_t gpio_catalog_count(bool include_advanced) {
    size_t count = 0;
    for (size_t k = 0; k < GPIO_CATALOG_TOTAL; k++) {
        if (gpio_catalog_visible(&k_catalog[k], include_advanced)) {
            count++;
        }
    }
    return count;
}

const GpioCatalogEntry* gpio_catalog_get(size_t i, bool include_advanced) {
    /* Enumerate SAFE entries first (array order), then ADVANCED (array order)
     * when included. This keeps SAFE pins on top regardless of how the array
     * is ordered, so index stability holds even if entries are reordered. */
    size_t seen = 0;

    /* Pass 1: SAFE entries. */
    for (size_t k = 0; k < GPIO_CATALOG_TOTAL; k++) {
        if (k_catalog[k].tier != GpioTierSafe) {
            continue;
        }
        if (seen == i) {
            return &k_catalog[k];
        }
        seen++;
    }

    if (!include_advanced) {
        return NULL;
    }

    /* Pass 2: ADVANCED entries. */
    for (size_t k = 0; k < GPIO_CATALOG_TOTAL; k++) {
        if (k_catalog[k].tier != GpioTierAdvanced) {
            continue;
        }
        if (seen == i) {
            return &k_catalog[k];
        }
        seen++;
    }

    return NULL;
}
