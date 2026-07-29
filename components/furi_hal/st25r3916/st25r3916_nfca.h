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

/** Switch the RF field off and disable the receiver/oscillator. */
void st25r3916_nfca_deinit(void);

/** Human-readable status string (for the test scene). */
const char* st25r3916_nfca_status_str(St25rNfcaStatus s);

#ifdef __cplusplus
}
#endif
