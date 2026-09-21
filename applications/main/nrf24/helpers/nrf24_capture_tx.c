/* nrf24_capture.h's TX/replay half — split into its own translation unit so
 * the pure capture store (nrf24_capture.c) and its host test stay furi-free.
 * This file pulls in nrf24_hw.h (real SPI/GPIO access via furi_hal), so it is
 * firmware-only; it is never compiled into the host test harness.
 *
 * Whole file is attack-tooling and PRIVATE-only, same as scene_sniffer.c (its
 * only caller) — don't rely on --gc-sections to drop it from a public build,
 * assert it explicitly like every other gated file in this tree. */
#ifndef PUBLIC_BUILD

#include "nrf24_capture.h"
#include "../nrf24_hw.h"

/* Common burst implementation. `abort`, if non-NULL, is polled between sends
 * so a caller (e.g. a UI Back handler) can cut a multi-repeat burst short. */
static void nrf24_tx_raw_ex(
    const uint8_t addr[5],
    uint8_t chan,
    const uint8_t* payload,
    uint8_t len,
    uint8_t repeats,
    volatile bool* abort) {
    if(addr == NULL || payload == NULL || len == 0) return;
    if(len > 32) len = 32;
    if(repeats == 0) repeats = 1;

    nrf24_hw_init();
    nrf24_hw_acquire();

    /* Register setup mirrors nrf24_mj_core.c's mj_setup_tx_for_target(): 5-byte
     * ESB address, AutoAck off, no retries, 2 Mbps, max PA, CRC off. */
    nrf24_hw_tx_setup(addr, 5, chan, len);

    for(uint8_t i = 0; i < repeats; i++) {
        if(abort && *abort) break;
        nrf24_hw_tx_send(payload, len);
    }

    nrf24_hw_tx_stop();
    nrf24_hw_release();
    nrf24_hw_deinit();
}

void nrf24_tx_raw(
    const uint8_t addr[5],
    uint8_t chan,
    const uint8_t* payload,
    uint8_t len,
    uint8_t repeats) {
    nrf24_tx_raw_ex(addr, chan, payload, len, repeats, NULL);
}

/* Abort-aware variant used by scene_sniffer.c's Replaying state so Back can
 * cut a burst short mid-flight. Not part of nrf24_capture.h's public surface
 * (forward-declared directly in scene_sniffer.c, the only caller) since it's
 * a UI-abort detail, not a generic capture-engine primitive. */
void nrf24_capture_replay_abortable(
    const Nrf24Capture* c,
    uint8_t repeats,
    volatile bool* abort) {
    if(c == NULL) return;
    nrf24_tx_raw_ex(c->addr, c->channel, c->payload, c->len, repeats, abort);
}

#endif /* !PUBLIC_BUILD -- attack tooling, private only */
