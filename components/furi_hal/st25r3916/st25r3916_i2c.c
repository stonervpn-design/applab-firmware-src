/**
 * @file st25r3916_i2c.c
 * @brief ST25R3916 NFC frontend — I2C transport + chip-ID probe (bring-up).
 *
 * See st25r3916_i2c.h. Task 1: prove I2C comms + read the chip-identity
 * register. Shares the Grove NFC I2C bus with the PN532 backend; does NOT
 * modify or replace the PN532 path (that is Task 2).
 *
 * ST25R3916 I2C protocol (same command set as its SPI mode — the first byte
 * after the I2C device address is the "operation-mode" byte):
 *   - Register write : mode-byte = (reg & 0x3F) | 0x00 ; followed by data bytes
 *   - Register read  : mode-byte = (reg & 0x3F) | 0x40 ; then repeated-START read
 *   - Direct command : mode-byte = (cmd & 0x3F) | 0xC0 (cmd already >= 0xC0)
 *   - Space-B access : prefix the direct command 0xFB (SPACE_B_ACCESS), then the
 *                      mode-byte with the SPACE_B (0x40) indicator cleared.
 * Reference: Flipper firmware lib/drivers/st25r3916_reg.c (SPI transport); the
 * operation-mode byte is bus-agnostic per the ST25R3916 datasheet.
 */

#include "st25r3916_i2c.h"
#include "st25r3916_reg.h"

#include <furi.h>
#include <board.h>

#define TAG "St25r3916"

#if defined(BOARD_HAS_ST25R3916) && BOARD_HAS_ST25R3916

#include <driver/i2c.h>
#include <driver/gpio.h>
#include <string.h>

/* ── ST25R3916 operation-mode byte (bits [7:6]) ─────────────────────────────
 * Mirrors the SPI mode bytes from Flipper's st25r3916_reg.c; identical on I2C. */
#define ST25R3916_MODE_WRITE (0U << 6) /* 0x00 : register write */
#define ST25R3916_MODE_READ  (1U << 6) /* 0x40 : register read  */
#define ST25R3916_MODE_CMD   (3U << 6) /* 0xC0 : direct command */

/* Expected IC_IDENTITY ic_type field for a genuine ST25R3916 (0x05). */
#define ST25R3916_IC_TYPE_EXPECTED \
    (ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916 >> ST25R3916_REG_IC_IDENTITY_ic_type_shift)

#ifndef BOARD_ST25R3916_I2C_ADDR
#define BOARD_ST25R3916_I2C_ADDR 0x50
#endif

#define ST25R3916_I2C_TIMEOUT_MS 50

esp_err_t st25r3916_i2c_init(void) {
    /* Shared with the PN532 path / power HAL. Guard the (re-)install exactly
     * like pn532_i2c_init(): install fresh, or reuse an already-running bus. */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_NFC_SDA,
        .scl_io_num = BOARD_PIN_NFC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000, /* 200kHz: reliable 2x over stock 100kHz */
    };

    esp_err_t err = i2c_driver_install(BOARD_NFC_I2C_PORT, conf.mode, 0, 0, 0);
    if(err == ESP_OK) {
        /* Fresh install: apply pins + 200 kHz ONCE, before any traffic (so no
         * glitch). 200 kHz is the reliable middle ground — 400 kHz corrupted FIFO
         * reads on the Grove-cabled M5 NFC unit (EMV -> "unknown"), 200 kHz has
         * margin and, with burst IRQ/FIFO reads, cuts detection + read latency. */
        i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
        return ESP_OK;
    }
    if(err == ESP_ERR_INVALID_STATE || err == ESP_FAIL) {
        /* Already installed on this shared bus (a prior st25r/pn532 init, or the
         * diag's bus bring-up). CRITICAL: the ESP-IDF 5.4 legacy I2C driver
         * returns ESP_FAIL — NOT ESP_ERR_INVALID_STATE — for a double
         * i2c_driver_install. The old code only special-cased INVALID_STATE, so it
         * treated "already installed" as a hard failure and returned it; probe()
         * then bailed at `if(st25r3916_i2c_init()!=ESP_OK) return false;` on EVERY
         * call after the first boot probe (which alone hits the fresh path above).
         * That made the chip read "absent" post-boot even though it's a genuine,
         * fully-readable ST25R3916 (check_chip_id returns ic_type=5). Reuse the
         * already-configured bus, but RE-ASSERT our 200 kHz first: when the primary
         * ST25 probe misses (module still on the 150 ms boost-settle / Grove glitch),
         * furi_hal_nfc_init() falls through to pn532_i2c_init(), which reprograms this
         * shared bus to 100 kHz and never restores it. The secondary ST25 probe then
         * succeeds via this reuse branch and the chip runs the WHOLE session at half
         * speed — doubling the I2C wire-time of every MIFARE auth, so a dict/mfkey
         * sweep crawls. Re-param_config here mirrors exactly what pn532_i2c_init() does
         * at 100 kHz every boot; probe()'s 4x IC-ID retry absorbs any single glitched
         * read, and this path only runs at init/chip_init, never mid-transaction. On a
         * real-PN532 board the PN532's own 100 kHz force runs after this, so it wins. */
        i2c_param_config(BOARD_NFC_I2C_PORT, &conf); /* conf.clk_speed = 200000 */
        return ESP_OK;
    }
    /* A genuine failure (e.g. ESP_ERR_INVALID_ARG / bad pin config) on a fresh
     * attempt — surface it instead of masking it. */
    FURI_LOG_E(TAG, "I2C bus %d driver install failed: %s", BOARD_NFC_I2C_PORT,
        esp_err_to_name(err));
    return err;
}

esp_err_t st25r3916_i2c_set_clock(uint32_t clk_hz) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_NFC_SDA,
        .scl_io_num = BOARD_PIN_NFC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = clk_hz,
    };
    return i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
}

esp_err_t st25r3916_read_reg(uint8_t reg, uint8_t* val) {
    if(!val) return ESP_ERR_INVALID_ARG;

    /* Build the write phase: optional SPACE_B_ACCESS prefix + read mode-byte. */
    uint8_t cmd[2];
    size_t cmd_len = 0;
    if(reg & ST25R3916_SPACE_B) {
        cmd[cmd_len++] = ST25R3916_CMD_SPACE_B_ACCESS; /* 0xFB direct command */
    }
    cmd[cmd_len++] = (uint8_t)((reg & ~ST25R3916_SPACE_B) | ST25R3916_MODE_READ);

    /* write mode-byte(s) then repeated-START read one data byte */
    return i2c_master_write_read_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        cmd,
        cmd_len,
        val,
        1,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_read_regs(uint8_t reg, uint8_t* vals, uint8_t n) {
    if(!vals || n == 0) return ESP_ERR_INVALID_ARG;

    /* Burst read: write one read mode-byte, then read n bytes. The ST25R3916
     * auto-increments the register address across the read, so this returns
     * `reg`, `reg+1`, ... `reg+n-1` in a SINGLE I2C transaction instead of n.
     * Used for the consecutive IRQ status regs (0x1A..0x1C) and FIFO status
     * (0x1E..0x1F) — cutting the I2C traffic in the hot poll loop ~3x. */
    uint8_t cmd[2];
    size_t cmd_len = 0;
    if(reg & ST25R3916_SPACE_B) {
        cmd[cmd_len++] = ST25R3916_CMD_SPACE_B_ACCESS;
    }
    cmd[cmd_len++] = (uint8_t)((reg & ~ST25R3916_SPACE_B) | ST25R3916_MODE_READ);

    return i2c_master_write_read_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        cmd,
        cmd_len,
        vals,
        n,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_write_reg(uint8_t reg, uint8_t val) {
    /* [optional 0xFB][write mode-byte][data] */
    uint8_t buf[3];
    size_t len = 0;
    if(reg & ST25R3916_SPACE_B) {
        buf[len++] = ST25R3916_CMD_SPACE_B_ACCESS;
    }
    buf[len++] = (uint8_t)((reg & ~ST25R3916_SPACE_B) | ST25R3916_MODE_WRITE);
    buf[len++] = val;

    return i2c_master_write_to_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        buf,
        len,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

/* ── FIFO operation-mode bytes (bus-agnostic, ST25R3916 datasheet) ──────────
 * Top two bits = 10 selects FIFO. Load=0x80, Read=0x9F (PT-mem read is 0xBF —
 * do NOT confuse them). */
#define ST25R3916_FIFO_LOAD 0x80U
#define ST25R3916_FIFO_READ 0x9FU
/* Passive-target memory A-config load (card emulation): 0b1010_0000, writes
 * from PT_Memory location 0. (F-config=0xA8, TSN=0xAC, PT-mem read=0xBF.) */
#define ST25R3916_PTMEM_LOAD_A 0xA0U

esp_err_t st25r3916_cmd(uint8_t cmd) {
    return i2c_master_write_to_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        &cmd,
        1,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_write_fifo(const uint8_t* data, uint8_t len) {
    if(!data || len == 0) return ESP_ERR_INVALID_ARG;
    uint8_t buf[1 + 255];
    buf[0] = ST25R3916_FIFO_LOAD;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        buf,
        (size_t)len + 1,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_read_fifo(uint8_t* data, uint8_t len) {
    if(!data || len == 0) return ESP_ERR_INVALID_ARG;
    uint8_t mode = ST25R3916_FIFO_READ;
    return i2c_master_write_read_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        &mode,
        1,
        data,
        len,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_write_pta_mem(const uint8_t* data, uint8_t len) {
    if(!data || len == 0 || len > 15) return ESP_ERR_INVALID_ARG; /* PT_Memory-A is 15 bytes */
    uint8_t buf[1 + 15];
    buf[0] = ST25R3916_PTMEM_LOAD_A;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(
        BOARD_NFC_I2C_PORT,
        BOARD_ST25R3916_I2C_ADDR,
        buf,
        (size_t)len + 1,
        pdMS_TO_TICKS(ST25R3916_I2C_TIMEOUT_MS));
}

esp_err_t st25r3916_check_chip_id(uint8_t* ic_type_out) {
    uint8_t identity = 0;
    esp_err_t err = st25r3916_read_reg(ST25R3916_REG_IC_IDENTITY, &identity);
    if(err != ESP_OK) return err;

    uint8_t ic_type = (uint8_t)((identity & ST25R3916_REG_IC_IDENTITY_ic_type_mask) >>
                                ST25R3916_REG_IC_IDENTITY_ic_type_shift);
    uint8_t ic_rev = (uint8_t)(identity & ST25R3916_REG_IC_IDENTITY_ic_rev_mask);
    FURI_LOG_D(
        TAG,
        "IC_IDENTITY=0x%02X ic_type=0x%02X ic_rev=0x%02X",
        identity,
        ic_type,
        ic_rev);

    if(ic_type_out) *ic_type_out = ic_type;
    return ESP_OK;
}

bool st25r3916_probe(void) {
    if(st25r3916_i2c_init() != ESP_OK) {
        return false;
    }

    /* Read IC_IDENTITY, retrying a few times: I2C over the Grove cable can drop an
     * occasional transaction, and a spurious NAK here would wrongly report the chip
     * absent. (The "always absent after the first boot probe" bug was the ESP_FAIL
     * double-install return handling in st25r3916_i2c_init() above, now fixed; this
     * retry is just belt-and-suspenders against transient reads.) */
    uint8_t ic_type = 0xFF;
    esp_err_t err = ESP_FAIL;
    for(int attempt = 0; attempt < 4; attempt++) {
        err = st25r3916_check_chip_id(&ic_type);
        if(err == ESP_OK && ic_type == ST25R3916_IC_TYPE_EXPECTED) {
            FURI_LOG_I(
                TAG,
                "chip-id probe @0x%02X: ic_type=0x%02X -> ST25R3916 present (attempt %d)",
                BOARD_ST25R3916_I2C_ADDR,
                ic_type,
                attempt);
            return true;
        }
        furi_delay_ms(2);
    }

    FURI_LOG_I(
        TAG,
        "No ST25R3916 match at 0x%02X after retries (last err=%s ic_type=0x%02X exp 0x%02X)",
        BOARD_ST25R3916_I2C_ADDR,
        esp_err_to_name(err),
        ic_type,
        ST25R3916_IC_TYPE_EXPECTED);
    return false;
}

#else /* !BOARD_HAS_ST25R3916 */

esp_err_t st25r3916_i2c_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_i2c_set_clock(uint32_t clk_hz) {
    (void)clk_hz;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_read_reg(uint8_t reg, uint8_t* val) {
    (void)reg;
    (void)val;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_read_regs(uint8_t reg, uint8_t* vals, uint8_t n) {
    (void)reg;
    (void)vals;
    (void)n;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_write_reg(uint8_t reg, uint8_t val) {
    (void)reg;
    (void)val;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_check_chip_id(uint8_t* ic_type_out) {
    (void)ic_type_out;
    return ESP_ERR_NOT_SUPPORTED;
}
bool st25r3916_probe(void) {
    return false;
}
esp_err_t st25r3916_cmd(uint8_t cmd) {
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_write_fifo(const uint8_t* data, uint8_t len) {
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_read_fifo(uint8_t* data, uint8_t len) {
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t st25r3916_write_pta_mem(const uint8_t* data, uint8_t len) {
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BOARD_HAS_ST25R3916 */
