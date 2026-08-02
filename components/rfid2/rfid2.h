#pragma once

// M5Stack Unit RFID2 driver — WS1850S (MFRC522-register-compatible), 13.56 MHz,
// I2C @ 0x28. Reads ISO14443-A card UIDs (MIFARE Classic/Ultralight, NTAG).
//
// Connects to the Cardputer-ADV side Grove PORT (HY2.0): SDA=G2, SCL=G1 — a
// SEPARATE I2C bus from the internal keyboard bus (G8/G9). The driver initializes
// this bus itself on a free I2C peripheral. (G1 is shared with IR-RX, so don't use
// IR while the RFID2 is plugged in.)
//
// NOT hardware-tested — standard MFRC522 sequences; validate on the unit.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <driver/i2c.h>

#include <boards/board.h> // BOARD_PIN_QWIIC_* Grove pins (board-specific)

#define RFID2_I2C_ADDR 0x28
#define RFID2_I2C_PORT I2C_NUM_1 // dedicated bus for the Grove port
// Grove I2C pins come from the board's QWIIC/Grove defines so this driver lands
// on the right connector per board (Cardputer-ADV → G2/G1; StickS3 → G9/G10, which
// keeps it OFF the StickS3 radio pins G2=CC1101_CSN / G1=nRF24_CE). Falls back to
// the Cardputer-ADV Grove pins on boards that don't define QWIIC.
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define RFID2_SDA_PIN  BOARD_PIN_QWIIC_SDA
#define RFID2_SCL_PIN  BOARD_PIN_QWIIC_SCL
#else
#define RFID2_SDA_PIN  2 // Cardputer-ADV Grove SDA (G2)
#define RFID2_SCL_PIN  1 // Cardputer-ADV Grove SCL (G1)
#endif

/** Soft-reset + configure the reader and turn the antenna on. false on failure. */
bool rfid2_init(void);

/** Read the MFRC522 VersionReg (sanity check the chip responds; 0x00/0xFF = no chip). */
uint8_t rfid2_version(void);

/** Diagnostic: probe every I2C address on the Grove bus; fill `out` with the 7-bit
 * addresses that ACK, return the count (or -1 if the bus can't be brought up). */
int rfid2_scan(uint8_t* out, int max);

/**
 * Poll once for a card in the field. If present, fills uid (4/7/10 bytes),
 * uid_len and sak, and returns true. Returns false if no card.
 * (Anticollision only — does NOT SELECT the card. Use rfid2_select for reads.)
 */
bool rfid2_read_uid(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak);

/**
 * Full ISO14443-3A activation: REQA + anticollision cascade + SELECT, leaving the
 * card in the ACTIVE state (required before MIFARE auth / READ). Fills uid, uid_len
 * and the final SAK. Returns false if no card / activation failed.
 */
bool rfid2_select(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak);

/**
 * MIFARE Classic Crypto1 authenticate `block` with a 6-byte key (key B if keyB,
 * else key A), using the selected card's uid. The card must already be selected
 * (rfid2_select). Returns true if authenticated. Stays in crypto until rfid2_halt.
 */
bool rfid2_classic_auth(
    uint8_t block,
    const uint8_t key[6],
    bool keyB,
    const uint8_t* uid,
    size_t uid_len);

/**
 * Read 16 bytes at `block` (MIFARE Classic block after auth, or Ultralight/NTAG
 * READ 0x30 which returns 4 pages). Returns true on success.
 */
bool rfid2_read16(uint8_t block, uint8_t out[16]);

/** HALT the card and turn Crypto1 off, so a fresh card can be selected. */
bool rfid2_halt(void);

/**
 * Low-level ISO14443-A transceive for the unified furi_hal_nfc MFRC522 backend.
 * Sends `tx_len` bytes (`tx_last_bits` = valid bits in the final byte for short
 * frames like the 7-bit REQA; 0 = whole bytes), collects up to `rx_max` reply
 * bytes. The caller owns CRC (none is appended on TX; RX is returned raw,
 * including the card's CRC). Returns the reply byte count, or <=0 on no card /
 * error.
 */
int rfid2_transceive(const uint8_t* tx, int tx_len, int tx_last_bits, uint8_t* rx, int rx_max);

/** Turn the RF field (antenna drivers) off for low-power; rfid2_init turns it back on. */
void rfid2_field_off(void);

/**
 * One-shot full card read. Selects the card in the field, identifies it by SAK,
 * and dumps everything readable into `text` as newline-separated hex rows
 * (MIFARE Classic: per-block, trying a built-in default-key dictionary key A then
 * B per sector; Ultralight/NTAG: per-4-page READ). Fills uid/uid_len/sak and a
 * short human type string. Returns the number of data rows written, or -1 if no
 * card / select failed.
 */
int rfid2_dump_text(
    char* text,
    size_t text_size,
    uint8_t* uid,
    size_t uid_max,
    size_t* uid_len,
    uint8_t* sak,
    char* type_out,
    size_t type_size);
