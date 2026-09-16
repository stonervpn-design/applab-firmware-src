/**
 * @file furi_hal_resources.c
 * Resources HAL — pin definitions driven by board header
 */

#include "furi_hal_resources.h"
#include "furi_hal_infrared.h"
#include "furi_hal_spi.h"
#include "furi_hal_spi_types.h"
#include "boards/board.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>

const InputPin input_pins[] = {};
const size_t input_pins_count = 0;

/* Hardware Button Pins */
const GpioPin gpio_button_boot  = {.port = NULL, .pin = BOARD_PIN_BUTTON_BOOT};
const GpioPin gpio_battery_sense = {.port = NULL, .pin = BOARD_PIN_BATTERY_ADC};

/* LCD Pins */
const GpioPin gpio_lcd_din = {.port = NULL, .pin = BOARD_PIN_LCD_MOSI};
const GpioPin gpio_lcd_clk = {.port = NULL, .pin = BOARD_PIN_LCD_SCLK};
const GpioPin gpio_lcd_dc  = {.port = NULL, .pin = BOARD_PIN_LCD_DC};
const GpioPin gpio_lcd_cs  = {.port = NULL, .pin = BOARD_PIN_LCD_CS};
const GpioPin gpio_lcd_rst = {.port = NULL, .pin = BOARD_PIN_LCD_RST};
const GpioPin gpio_lcd_bl  = {.port = NULL, .pin = BOARD_PIN_LCD_BL};

/* SD Card Pins */
const GpioPin gpio_sdcard_cs   = {.port = NULL, .pin = BOARD_PIN_SD_CS};
const GpioPin gpio_sdcard_miso = {.port = NULL, .pin = BOARD_PIN_SD_MISO};

/* Touch Pins */
const GpioPin gpio_touch_scl = {.port = NULL, .pin = BOARD_PIN_TOUCH_SCL};
const GpioPin gpio_touch_sda = {.port = NULL, .pin = BOARD_PIN_TOUCH_SDA};
const GpioPin gpio_touch_rst = {.port = NULL, .pin = BOARD_PIN_TOUCH_RST};
const GpioPin gpio_touch_int = {.port = NULL, .pin = BOARD_PIN_TOUCH_INT};

/* External / CC1101 pins */
const GpioPin gpio_ext_pc0  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pc1  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pc3  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pb2  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_ext_pb3  = {.port = NULL, .pin = BOARD_PIN_CC1101_SCK};
/* CSN / GDO0 / NRF24 CSN are non-const so the Pin Config app can remap them at
 * boot; every consumer reads .pin on-demand when it opens the device. */
GpioPin gpio_ext_pa4  = {.port = NULL, .pin = BOARD_PIN_CC1101_CSN};
const GpioPin gpio_ext_pa6  = {.port = NULL, .pin = BOARD_PIN_CC1101_MISO};
const GpioPin gpio_ext_pa7  = {.port = NULL, .pin = BOARD_PIN_CC1101_MOSI};
GpioPin gpio_cc1101_g0 = {.port = NULL, .pin = BOARD_PIN_CC1101_GDO0};

#ifdef BOARD_PIN_NRF24_CSN
GpioPin gpio_nrf24_cs = {.port = NULL, .pin = BOARD_PIN_NRF24_CSN};
#else
GpioPin gpio_nrf24_cs = {.port = NULL, .pin = UINT16_MAX};
#endif
const GpioPin gpio_ibutton  = {.port = NULL, .pin = UINT16_MAX};
const GpioPin gpio_speaker  = {.port = NULL, .pin = UINT16_MAX};

#ifdef BOARD_PIN_NRF24_CE
uint16_t furi_hal_nrf24_ce_pin = BOARD_PIN_NRF24_CE;
#else
uint16_t furi_hal_nrf24_ce_pin = UINT16_MAX;
#endif

void furi_hal_resources_set_rf_pins(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce) {
    gpio_ext_pa4.pin = cc1101_csn;
    gpio_cc1101_g0.pin = cc1101_gdo0;
    gpio_nrf24_cs.pin = nrf24_csn;
    furi_hal_nrf24_ce_pin = nrf24_ce;
}

/* -------------------------------------------------------------------------
 * Runtime-movable module pins, persisted in NVS. Applied from furi_hal_init()
 * BEFORE furi_hal_subghz_init() so the CC1101 boot probe uses the configured
 * CSN/GDO0. NVS (unlike the SD card) is available this early in boot.
 * -------------------------------------------------------------------------*/
#define MODPINS_NS "modpins"

static uint8_t modpins_get(nvs_handle_t h, const char* key, uint8_t def) {
    uint8_t v = def;
    nvs_get_u8(h, key, &v);
    return v;
}

void furi_hal_module_pins_apply_nvs(void) {
    nvs_handle_t h;
    if(nvs_open(MODPINS_NS, NVS_READONLY, &h) != ESP_OK) return; /* none saved -> defaults */

    gpio_ext_pa4.pin = modpins_get(h, "csn", (uint8_t)BOARD_PIN_CC1101_CSN);
    gpio_cc1101_g0.pin = modpins_get(h, "gdo0", (uint8_t)BOARD_PIN_CC1101_GDO0);
#ifdef BOARD_PIN_NRF24_CSN
    gpio_nrf24_cs.pin = modpins_get(h, "ncsn", (uint8_t)BOARD_PIN_NRF24_CSN);
#endif
    furi_hal_nrf24_ce_pin = modpins_get(h, "nce", (uint8_t)furi_hal_nrf24_ce_pin);
    uint8_t ir_tx = modpins_get(h, "irtx", (uint8_t)BOARD_PIN_IR_TX);
    uint8_t ir_rx = modpins_get(h, "irrx", (uint8_t)BOARD_PIN_IR_RX);
    nvs_close(h);

    furi_hal_infrared_set_pins(ir_tx, ir_rx);
}

void furi_hal_module_pins_read_nvs(
    uint8_t* cc1101_csn,
    uint8_t* cc1101_gdo0,
    uint8_t* nrf24_csn,
    uint8_t* nrf24_ce,
    uint8_t* ir_tx,
    uint8_t* ir_rx) {
    /* Report the authoritative persisted values (NVS, falling back to board
     * defaults) so the Pin Config UI shows exactly what the hardware uses — no
     * separate SD display store that can drift out of sync. */
    *cc1101_csn = (uint8_t)BOARD_PIN_CC1101_CSN;
    *cc1101_gdo0 = (uint8_t)BOARD_PIN_CC1101_GDO0;
#ifdef BOARD_PIN_NRF24_CSN
    *nrf24_csn = (uint8_t)BOARD_PIN_NRF24_CSN;
#else
    *nrf24_csn = (uint8_t)furi_hal_nrf24_ce_pin; /* placeholder; no NRF24 CSN macro */
#endif
    *nrf24_ce = (uint8_t)furi_hal_nrf24_ce_pin;
    *ir_tx = (uint8_t)BOARD_PIN_IR_TX;
    *ir_rx = (uint8_t)BOARD_PIN_IR_RX;

    nvs_handle_t h;
    if(nvs_open(MODPINS_NS, NVS_READONLY, &h) != ESP_OK) return; /* none saved -> defaults */
    *cc1101_csn = modpins_get(h, "csn", *cc1101_csn);
    *cc1101_gdo0 = modpins_get(h, "gdo0", *cc1101_gdo0);
    *nrf24_csn = modpins_get(h, "ncsn", *nrf24_csn);
    *nrf24_ce = modpins_get(h, "nce", *nrf24_ce);
    *ir_tx = modpins_get(h, "irtx", *ir_tx);
    *ir_rx = modpins_get(h, "irrx", *ir_rx);
    nvs_close(h);
}

void furi_hal_module_pins_save_nvs_only(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce,
    uint16_t ir_tx,
    uint16_t ir_rx) {
    /* Persist to NVS ONLY. The running GpioPin structs are left untouched, so this
     * never drives a GPIO live — the new pins take effect only at the next boot,
     * when furi_hal_init reads NVS back. This is what lets the Pin Config app move a
     * radio onto a pin without instantly reconfiguring it (e.g. clobbering the
     * TCA8418 keyboard bus on GPIO 8/9). */
    nvs_handle_t h;
    esp_err_t oerr = nvs_open(MODPINS_NS, NVS_READWRITE, &h);
    if(oerr == ESP_OK) {
        nvs_set_u8(h, "csn", (uint8_t)cc1101_csn);
        nvs_set_u8(h, "gdo0", (uint8_t)cc1101_gdo0);
        nvs_set_u8(h, "ncsn", (uint8_t)nrf24_csn);
        nvs_set_u8(h, "nce", (uint8_t)nrf24_ce);
        nvs_set_u8(h, "irtx", (uint8_t)ir_tx);
        nvs_set_u8(h, "irrx", (uint8_t)ir_rx);
        esp_err_t cerr = nvs_commit(h);
        nvs_close(h);
        if(cerr != ESP_OK)
            ESP_LOGE("modpins", "nvs_commit failed: %d (%s)", cerr, esp_err_to_name(cerr));
    } else {
        /* Surface the failure instead of silently dropping the save (the old
         * behaviour that made pin edits look like they didn't persist). */
        ESP_LOGE("modpins", "nvs_open(RW) failed: %d (%s)", oerr, esp_err_to_name(oerr));
    }
}

void furi_hal_module_pins_save_nvs(
    uint16_t cc1101_csn,
    uint16_t cc1101_gdo0,
    uint16_t nrf24_csn,
    uint16_t nrf24_ce,
    uint16_t ir_tx,
    uint16_t ir_rx) {
    furi_hal_module_pins_save_nvs_only(
        cc1101_csn, cc1101_gdo0, nrf24_csn, nrf24_ce, ir_tx, ir_rx);
    /* apply to the running structs immediately (full effect on reboot) */
    furi_hal_resources_set_rf_pins(cc1101_csn, cc1101_gdo0, nrf24_csn, nrf24_ce);
    furi_hal_infrared_set_pins(ir_tx, ir_rx);
}

/* Live NRF24 presence check: write RF_CH twice with distinct values and read
 * back — a real nRF24 echoes them, so it confirms the chip on the current CSN.
 * Only needs SPI (no CE), so it works from the Pin Config status view. */
bool furi_hal_nrf24_is_connected(void) {
    const uint8_t NRF_W_REG = 0x20, NRF_R_REG = 0x00, NRF_RF_CH = 0x05;
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);

    uint8_t tx[2], rx[2];
    bool ok = true;
    const uint8_t probes[2] = {0x55, 0x2A};
    for(int i = 0; i < 2 && ok; i++) {
        tx[0] = NRF_W_REG | NRF_RF_CH;
        tx[1] = probes[i];
        furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
        tx[0] = NRF_R_REG | NRF_RF_CH;
        tx[1] = 0xFF;
        rx[0] = rx[1] = 0;
        furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, sizeof(tx), 100);
        ok = (rx[1] == probes[i]);
    }

    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
    return ok;
}

/* Generic NVS-backed blob store (survives reboot AND firmware flash — NVS is its
 * own partition). Used where /int (spiffs) would be, but that partition doesn't
 * exist on this board. */
#define FURI_HAL_PERSIST_NS "furihalkv"

/* NVS keys must be <= 15 chars; truncate defensively so callers can't silently
 * fail with ESP_ERR_NVS_KEY_TOO_LONG. */
static void persist_key(char out[16], const char* key) {
    size_t i = 0;
    for(; key[i] && i < 15; i++) out[i] = key[i];
    out[i] = '\0';
}

bool furi_hal_persist_get(const char* key_in, void* out, size_t size) {
    char key[16];
    persist_key(key, key_in);
    nvs_handle_t h;
    if(nvs_open(FURI_HAL_PERSIST_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = size;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == size;
}

void furi_hal_persist_set(const char* key_in, const void* data, size_t size) {
    char key[16];
    persist_key(key, key_in);
    nvs_handle_t h;
    if(nvs_open(FURI_HAL_PERSIST_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE("persist", "set '%s': nvs_open failed", key);
        return;
    }
    esp_err_t serr = nvs_set_blob(h, key, data, size);
    esp_err_t cerr = (serr == ESP_OK) ? nvs_commit(h) : serr;
    nvs_close(h);
    if(serr != ESP_OK || cerr != ESP_OK) {
        ESP_LOGE(
            "persist", "set '%s' failed: set=%d (%s) commit=%d", key, serr, esp_err_to_name(serr),
            cerr);
    }
}
