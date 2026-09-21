/**
 * @file furi_hal_resources.h
 * Resources HAL API (ESP32 stub)
 */

#pragma once

#include <stddef.h>
#include <furi_hal_gpio.h>
#include <input.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input Related Constants */
#define INPUT_DEBOUNCE_TICKS 4

/* Light */
typedef enum {
    LightRed = (1 << 0),
    LightGreen = (1 << 1),
    LightBlue = (1 << 2),
    LightBacklight = (1 << 3),
} Light;

typedef struct {
    const GpioPin* gpio;
    const InputKey key;
    const bool inverted;
    const char* name;
} InputPin;

extern const InputPin input_pins[];
extern const size_t input_pins_count;

/* Hardware Button Pins (Waveshare ESP32-C6-Touch-LCD-1.9) */
extern const GpioPin gpio_button_boot; /* GPIO9 - BOOT button (active low) */
extern const GpioPin gpio_battery_sense; /* GPIO0 - BAT_ADC (VCC / 3) */

/* LCD Pins (Waveshare ESP32-C6-LCD-1.9, ST7789V2) */
extern const GpioPin gpio_lcd_din;  /* GPIO4  - SPI MOSI */
extern const GpioPin gpio_lcd_clk;  /* GPIO5  - SPI SCLK */
extern const GpioPin gpio_lcd_dc;   /* GPIO6  - Data/Command */
extern const GpioPin gpio_lcd_cs;   /* GPIO7  - Chip Select */
extern const GpioPin gpio_lcd_rst;  /* GPIO14 - Reset */
extern const GpioPin gpio_lcd_bl;   /* GPIO15 - Backlight (PWM) */

/* SD Card Pins (Waveshare ESP32-C6-LCD-1.9, shared SPI2_HOST) */
extern const GpioPin gpio_sdcard_cs;   /* GPIO20 - SD Chip Select */
extern const GpioPin gpio_sdcard_miso; /* GPIO19 - SD MISO */

/* Touch Pins (Waveshare ESP32-C6-LCD-1.9, CST816S via I2C) */
extern const GpioPin gpio_touch_scl;   /* GPIO8  - I2C SCL */
extern const GpioPin gpio_touch_sda;   /* GPIO18 - I2C SDA */
extern const GpioPin gpio_touch_rst;   /* GPIO21 - Reset */
extern const GpioPin gpio_touch_int;   /* GPIO22 - Interrupt (active low) */

/* Flipper-style external header pins.
 * Current mapping is dedicated to the external CC1101 bring-up and overlaps with
 * some earlier Waveshare board assumptions for touch/SD.
 */
extern const GpioPin gpio_ext_pc0;
extern const GpioPin gpio_ext_pc1;
extern const GpioPin gpio_ext_pc3;
extern const GpioPin gpio_ext_pb2;
extern const GpioPin gpio_ext_pb3; /* CC1101 SCK  - GPIO2 */
extern GpioPin gpio_ext_pa4; /* CC1101 CSN - runtime-overridable via Pin Config */
extern const GpioPin gpio_ext_pa6; /* CC1101 MISO - GPIO17 */
extern const GpioPin gpio_ext_pa7; /* CC1101 MOSI - GPIO3 */
extern GpioPin gpio_cc1101_g0; /* CC1101 GDO0 - runtime-overridable via Pin Config */
extern GpioPin gpio_nrf24_cs; /* NRF24 CSN - runtime-overridable via Pin Config */
extern const GpioPin gpio_ibutton;
extern const GpioPin gpio_speaker;

/* Runtime NRF24 CE pin (default = board macro), overridable via the Pin Config app. */
extern uint16_t furi_hal_nrf24_ce_pin;

/* Apply runtime pin overrides for the movable EXT-bus pins. Called at boot from the
 * Pin Config startup hook after the SD card (where the config lives) is mounted. */
void furi_hal_resources_set_rf_pins(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce);

/* Load movable module pins from NVS and apply. Call in furi_hal_init() BEFORE
 * furi_hal_subghz_init() so the CC1101 boot probe uses the configured pins. */
void furi_hal_module_pins_apply_nvs(void);

/* Live NRF24 presence check over SPI using the current NRF24 CSN. */
bool furi_hal_nrf24_is_connected(void);

/* Generic NVS-backed persistence (survives reboot and firmware flash). get
 * returns true only if the key exists and its stored size matches `size`. */
bool furi_hal_persist_get(const char* key, void* out, size_t size);
void furi_hal_persist_set(const char* key, const void* data, size_t size);

/* Read the persisted movable module pins from NVS (board defaults if unset). Lets
 * the Pin Config app display the authoritative values instead of a separate store. */
void furi_hal_module_pins_read_nvs(
    uint8_t* cc1101_csn,
    uint8_t* cc1101_gdo0,
    uint8_t* nrf24_csn,
    uint8_t* nrf24_ce,
    uint8_t* ir_tx,
    uint8_t* ir_rx);

/* Persist movable module pins to NVS ONLY (survives reboot). Does NOT touch the
 * running GpioPin structs, so nothing is driven live — the pins take effect at the
 * next boot. Used by the Pin Config app so editing a pin can never reconfigure a
 * GPIO mid-session (e.g. the TCA8418 keyboard bus on GPIO 8/9). */
void furi_hal_module_pins_save_nvs_only(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce,
    uint16_t ir_tx,
    uint16_t ir_rx);

/* Persist movable module pins to NVS (survives reboot) and apply immediately to
 * the running structs (full effect on reboot). */
void furi_hal_module_pins_save_nvs(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce,
    uint16_t ir_tx,
    uint16_t ir_rx);

#ifdef __cplusplus
}
#endif
