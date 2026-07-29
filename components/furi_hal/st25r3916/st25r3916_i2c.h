/**
 * @file st25r3916_i2c.h
 * @brief ST25R3916 NFC frontend — I2C transport + chip-ID probe (bring-up).
 *
 * Task 1 of the ST25R3916 driver port. Implements the ST25R3916's I2C access
 * protocol on top of ESP-IDF's `driver/i2c.h`, sharing the Grove NFC bus with
 * the existing PN532 backend (BOARD_NFC_I2C_PORT / BOARD_PIN_NFC_SDA/SCL).
 *
 * The ST25R3916 uses the same "operation-mode byte" command set on I2C as on
 * SPI: the first byte written after the I2C device address selects the access
 * type (register write / register read / direct command / FIFO / PT memory).
 * This file implements register read/write and a chip-identity probe; the FIFO
 * / transceive / IRQ layers are later tasks.
 *
 * These functions are standalone units (buildable + callable) and are NOT yet
 * wired into furi_hal_nfc.c — the PN532 backend remains the active NFC path.
 * See components/furi_hal/st25r3916/README-BRINGUP note in the task report.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the I2C bus for the ST25R3916.
 *
 * Shares BOARD_NFC_I2C_PORT with the PN532 backend. The bus may already be
 * installed (by furi_hal_power, the PN532 path, or a prior call); this guards
 * the (re-)install the same way the PN532 adapter does and treats an
 * already-installed bus as success.
 *
 * @return ESP_OK on success (fresh install or reuse), else an esp_err_t.
 */
esp_err_t st25r3916_i2c_init(void);

/**
 * Reconfigure the shared I2C bus clock (Hz). Used to raise the NFC bus to
 * fast-mode-plus (1 MHz) during card emulation to shrink the AUTH-response
 * latency, then restore 400 kHz afterwards. Affects every device on
 * BOARD_NFC_I2C_PORT, so only bump it while emulation owns the bus.
 */
esp_err_t st25r3916_i2c_set_clock(uint32_t clk_hz);

/**
 * Read a single ST25R3916 register.
 *
 * Frames the ST25R3916 I2C register-read: writes the operation-mode byte
 * (reg | READ_MODE) then reads one data byte via a repeated-start transaction.
 * Space-B registers (ST25R3916_SPACE_B bit set) are prefixed with the
 * SPACE_B_ACCESS direct command automatically.
 *
 * @param reg  register address (may carry the ST25R3916_SPACE_B flag)
 * @param val  out: register value
 * @return ESP_OK on success, else an esp_err_t (bus/NACK error).
 */
esp_err_t st25r3916_read_reg(uint8_t reg, uint8_t* val);

/**
 * Burst read of `n` consecutive registers starting at `reg`, in a single I2C
 * transaction (the ST25R3916 auto-increments the address across the read).
 * Used to read the IRQ status registers (0x1A..0x1C) and FIFO status
 * (0x1E..0x1F) in one go instead of n separate reads — the big I2C-latency win
 * in the hot poll loop.
 */
esp_err_t st25r3916_read_regs(uint8_t reg, uint8_t* vals, uint8_t n);

/**
 * Write a single ST25R3916 register.
 *
 * Frames the ST25R3916 I2C register-write: [mode-byte = reg | WRITE_MODE][val].
 * Space-B registers are prefixed with the SPACE_B_ACCESS direct command.
 *
 * @param reg  register address (may carry the ST25R3916_SPACE_B flag)
 * @param val  value to write
 * @return ESP_OK on success, else an esp_err_t.
 */
esp_err_t st25r3916_write_reg(uint8_t reg, uint8_t val);

/**
 * Read the IC_IDENTITY register (0x3F) and extract the ic_type field.
 *
 * @param ic_type_out  out: the 5-bit ic_type field (bits [7:3] shifted down).
 *                     For a genuine ST25R3916 this is 0x05
 *                     (ST25R3916_REG_IC_IDENTITY_ic_type_st25r3916 >> 3).
 * @return ESP_OK if the register was read (regardless of value), else esp_err_t.
 */
esp_err_t st25r3916_check_chip_id(uint8_t* ic_type_out);

/**
 * Probe whether an ST25R3916 responds at BOARD_ST25R3916_I2C_ADDR.
 *
 * Initializes the bus (guarded), reads IC_IDENTITY and checks the ic_type
 * field matches the ST25R3916 signature (0x05). Safe to call even when the
 * unit is a PN532 (returns false without disturbing the PN532 path).
 *
 * @return true if an ST25R3916 is present and identified, false otherwise.
 */
bool st25r3916_probe(void);

/**
 * Issue a direct command (single operation-mode byte, cmd already >= 0xC0).
 * @param cmd  one of the ST25R3916_CMD_* values.
 */
esp_err_t st25r3916_cmd(uint8_t cmd);

/**
 * Load the transmit FIFO. Frames the I2C write [FIFO_LOAD (0x80)][data...].
 * @param data  bytes to load (<= 96, the FIFO depth)
 * @param len   number of bytes
 */
esp_err_t st25r3916_write_fifo(const uint8_t* data, uint8_t len);

/**
 * Read the receive FIFO. Frames [FIFO_READ (0x9F)] then a repeated-START read
 * of @p len bytes. Caller sizes @p len from the FIFO status registers.
 */
esp_err_t st25r3916_read_fifo(uint8_t* data, uint8_t len);

/**
 * Load the passive-target memory A-config (card-emulation UID/ATQA/SAK).
 * Frames the I2C write [PTMEM_LOAD_A (0xA0)][data...] — @p data is written from
 * PT_Memory location 0 on (NFCID1 @0-9, SENS_RES/ATQA @10-11, SEL_RES @12-14).
 */
esp_err_t st25r3916_write_pta_mem(const uint8_t* data, uint8_t len);

#ifdef __cplusplus
}
#endif
