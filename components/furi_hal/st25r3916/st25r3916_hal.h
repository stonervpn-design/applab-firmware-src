/**
 * @file st25r3916_hal.h
 * @brief ST25R3916 poller engine for the furi_hal_nfc backend (Task 2).
 *
 * This is the transceive/field layer that `furi_hal_nfc.c` calls when the
 * runtime backend is the ST25R3916 (M5Stack NFC Universal Unit @ I2C 0x50).
 * It builds on the Task-1 I2C transport (`st25r3916_i2c.{c,h}`) and reuses the
 * proven REQA/anticollision register sequences from `st25r3916_nfca.c`, but is
 * factored so the upper NFC stack drives activation itself (short frame → SDD
 * frame → standard frame), exactly like the PN532 backend.
 *
 * CRC ownership: the software stack (iso14443_3a) appends CRC-A before TX and
 * checks/trims it after RX (iso14443_3a_poller_i.c:34/52/57). So the standard
 * data path here transmits WITHOUT hardware CRC and keeps the received CRC in
 * the FIFO (AUX.no_crc_rx=1) — pure pass-through. REQA/SDD frames carry no CRC.
 *
 * No IRQ line on the Grove 4-pin unit → completion is polled from the IRQ
 * status registers (0x1A/0x1B/0x1C), same hybrid model as the bring-up poller.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    St25rHalOk = 0, /**< Frame transmitted and a response was received. */
    St25rHalNoCard, /**< TX ok but no response (empty field / silence). */
    St25rHalError, /**< Framing/CRC/parity/protocol error or bus failure. */
} St25rHalStatus;

/** RF technology the poller front-end is configured for. Selected by the upper
 *  stack via furi_hal_nfc_set_mode(); applied to the chip in field_on. */
typedef enum {
    St25rTechNfcA = 0, /**< ISO14443-A 106k (default) */
    St25rTechNfcB, /**< ISO14443-B 106k */
    St25rTechFelica, /**< FeliCa (NFC-F) 212/424k */
    St25rTechNfcV, /**< ISO15693 (NFC-V) */
} St25rTech;

/** Select the RF technology for subsequent field_on/transceive. The actual
 *  chip reconfiguration happens in st25r3916_hal_field_on(). */
void st25r3916_hal_set_tech(St25rTech tech);

/** Non-destructive probe of the chip-identity register at 0x50. */
bool st25r3916_hal_present(void);

/** Force a full chip re-init on the next field_on (call after a possible replug). */
void st25r3916_hal_reset_ready(void);

/** Exact bit count of the most recent st25r3916_hal_transceive() RX frame
 *  (the last byte may be partial, e.g. a 4-bit WRITE/AUTH ACK). */
uint16_t st25r3916_hal_last_rx_bits(void);

/** Append an entry to the emulation trace ring-buffer (flushed over serial on
 *  the next field_off, when the CDC recovers). For diagnosing card emulation. */
void st25r3916_hal_emu_trace(const char* tag, const uint8_t* data, uint16_t n);

/** Reset + configure as an ISO14443-A poller (106 kbit/s) and switch the RF
 *  field on. Idempotent; safe to call at the start of every poll cycle.
 *  @returns St25rHalOk on success, St25rHalError on init failure. */
St25rHalStatus st25r3916_hal_field_on(void);

/** Switch the RF field off and disable the receiver/oscillator (low power). */
void st25r3916_hal_field_off(void);

/** Transmit an ISO14443-A short frame (REQA, or WUPA when @p wupa is true) and
 *  receive the ATQA. No CRC. */
St25rHalStatus
    st25r3916_hal_reqa(bool wupa, uint8_t* rx, uint16_t rx_cap, uint16_t* rx_len);

/** Transmit a byte-aligned frame WITHOUT hardware CRC and receive the raw
 *  answer (received CRC, if any, is left in the returned data for the software
 *  stack to verify). Used for both SDD/anticollision frames (no CRC involved)
 *  and standard data frames (software already appended the CRC to @p tx).
 *  @param rx_budget_ms  max time to wait for the response before NoCard. */
St25rHalStatus st25r3916_hal_transceive(
    const uint8_t* tx,
    uint16_t tx_len,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t rx_budget_ms);

/** Bit-granular transceive with hardware parity DISABLED, for MIFARE Classic
 *  Crypto1. @p tx already carries the encrypted parity bit after each data byte
 *  (9-bit groups, @p tx_bits = 9*N). Returns the raw received frame with an
 *  exact bit count in @p rx_bits (a multiple of 9) so the software cipher can
 *  recover data + parity. */
St25rHalStatus st25r3916_hal_transceive_custom_parity(
    const uint8_t* tx,
    uint16_t tx_bits,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_bits,
    uint32_t rx_budget_ms);

/* ── Listener / card emulation (NFC-A passive target) ─────────────────────── */

/** Event bits returned by st25r3916_hal_listen_poll(). */
#define ST25R_LISTEN_EV_ACTIVATED 0x01U /* reader completed SELECT (Active state) */
#define ST25R_LISTEN_EV_RX        0x02U /* a post-activation reader frame was received */
#define ST25R_LISTEN_EV_ENC       0x04U /* the received frame was encrypted (raw, no CRC) */

/** Configure the chip as an ISO14443-A passive target and load the PT memory
 *  with @p uid / @p atqa / @p sak. The chip then auto-answers REQA / full
 *  anticollision / SELECT (incl. SAK) in hardware. */
St25rHalStatus st25r3916_hal_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t* atqa,
    uint8_t sak);

/** Poll the passive target for up to @p timeout_ms. Sets @p out_events to a mask
 *  of ST25R_LISTEN_EV_*; on EV_RX fills @p rx/@p rx_len with the reader's frame.
 *  Returns St25rHalOk if any event fired, St25rHalNoCard on timeout. */
St25rHalStatus st25r3916_hal_listen_poll(
    uint8_t* out_events,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t timeout_ms);

/** Transmit a card-side response (load modulation). @p with_crc appends CRC-A. */
St25rHalStatus st25r3916_hal_listen_tx(const uint8_t* data, uint16_t len, bool with_crc);

/** Transmit a card-side response with CUSTOM parity (MIFARE Classic Crypto1):
 *  @p data are the (encrypted) bytes and @p parity is the bit-packed per-byte
 *  parity array (as from bit_buffer_get_parity). No CRC. */
St25rHalStatus st25r3916_hal_listen_tx_custom_parity(
    const uint8_t* data,
    const uint8_t* parity,
    uint16_t nbytes);

void st25r3916_hal_listen_sleep(void); /* GOTO_SLEEP (target → Halt) */
void st25r3916_hal_listen_sense(void); /* GOTO_SENSE (target → Sense/Idle) */
void st25r3916_hal_listen_stop(void); /* leave target mode, restore poller defaults */

#ifdef __cplusplus
}
#endif
