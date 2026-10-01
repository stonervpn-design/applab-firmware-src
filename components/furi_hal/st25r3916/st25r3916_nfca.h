/**
 * @file st25r3916_nfca.h
 * @brief Minimal ISO14443-A (NFC-A) reader on the ST25R3916 — enough to power
 *        the field, run REQA + single-card anticollision, and return a UID.
 *
 * This is the "prove it reads" layer (build FIFO/transceive/anticollision on
 * the bring-up transport, confirm the M5Stack ST25R3916 unit actually reads a
 * tag on real hardware) — NOT a full furi_hal_nfc backend yet.
 *
 * HARDWARE-VERIFICATION NOTES (this code compiles + links but is untested on
 * the unit): the analog receiver configuration after SET_DEFAULT is the most
 * likely tuning point — if the field powers on but no card ever answers
 * (status = NoCard) with a tag physically present, the RX/analog registers in
 * st25r3916_nfca_init() are the first thing to adjust. IRQ handling is polled
 * (the M5Stack unit exposes no IRQ line).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    St25rNfcaOk = 0,
    St25rNfcaErrInit, /* chip absent / SET_DEFAULT / oscillator never stabilized */
    St25rNfcaErrField, /* field could not be enabled */
    St25rNfcaErrNoCard, /* no ATQA — nothing in the field (normal when empty) */
    St25rNfcaErrAnticoll, /* REQA ok but anticollision/SELECT failed */
    St25rNfcaErrProto, /* framing/CRC/length error during activation */
} St25rNfcaStatus;

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len; /* 4, 7 or 10 */
    uint16_t atqa; /* ATQA (LSB first as received) */
    uint8_t sak; /* SAK of the last cascade level */
} St25rNfcaTag;

/** Reset + configure the ST25R3916 as an NFC-A poller and switch the RF field
 *  on. Returns St25rNfcaOk on success. Idempotent-ish: safe to call again. */
St25rNfcaStatus st25r3916_nfca_init(void);

/** One activation attempt: REQA -> anticollision -> SELECT. Fills @p out on
 *  success. Returns St25rNfcaErrNoCard when the field is empty. */
St25rNfcaStatus st25r3916_nfca_poll(St25rNfcaTag* out);

/** Post-activation poller data exchange (the transport's "standard frame"):
 *  transmit @p tx with CRC-A appended, then receive the card's answer (CRC-A
 *  checked + stripped) within a ~90 ms window sized for the Flipper-Share
 *  transport FWT. Call after a successful st25r3916_nfca_poll(). Returns
 *  St25rNfcaErrNoCard on silence, St25rNfcaErrProto on framing/CRC error. */
St25rNfcaStatus st25r3916_nfca_poller_txrx(
    const uint8_t* tx,
    uint8_t tx_len,
    uint8_t* rx,
    uint8_t rx_cap,
    uint8_t* rx_len);

/** Switch the RF field off and disable the receiver/oscillator. */
void st25r3916_nfca_deinit(void);

/** Human-readable status string (for the test scene). */
const char* st25r3916_nfca_status_str(St25rNfcaStatus s);

/* ─────────────────── NFC-A card emulation (passive target) ────────────────
 * Phase 1 of the Flipper Share NFC port: put the ST25R3916 into NFC-A passive
 * target mode so a reader (phone or a second unit's poller) can activate an
 * emulated card and exchange frames. The chip auto-answers REQA/anticoll/SELECT
 * from Passive Target Memory (loaded from the identity below); the MCU only
 * services post-activation data frames over the (sped-up) I2C link.
 */
typedef enum {
    St25rListenIdle = 0, /* armed, no external reader field yet */
    St25rListenField, /* a reader field is present */
    St25rListenActive, /* reader completed anticollision + SELECT */
    St25rListenRx, /* a data frame arrived (read it with _listen_rx) */
} St25rListenState;

/** Arm NFC-A passive-target emulation with the given card identity.
 *  @p uid_len must be 4, 7 or 10; @p atqa is sent LSB-first; @p sak is the SAK. */
St25rNfcaStatus st25r3916_nfca_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    uint16_t atqa,
    uint8_t sak);

/** Poll the target once. Updates *state; *raw_status (optional) returns the raw
 *  PASSIVE_TARGET_STATUS byte for on-hardware diagnosis. */
St25rNfcaStatus st25r3916_nfca_listen_poll(St25rListenState* state, uint8_t* raw_status);

/** Read a received reader frame from the FIFO (after St25rListenRx). */
St25rNfcaStatus st25r3916_nfca_listen_rx(uint8_t* buf, uint8_t cap, uint8_t* len);

/** Transmit a response frame with CRC-A (passive load modulation). */
St25rNfcaStatus st25r3916_nfca_listen_tx(const uint8_t* buf, uint8_t len);

/** Stop emulation: field/receiver off, back to idle. */
void st25r3916_nfca_listen_stop(void);

/** State name for the test scene. */
const char* st25r3916_nfca_listen_state_str(St25rListenState s);

#ifdef __cplusplus
}
#endif
