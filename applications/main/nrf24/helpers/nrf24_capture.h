#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* nRF24 capture engine — shared record/bounded-store used by the Sniffer,
 * KeySniffer, Drone hijack, and Fuzzer feature layers.
 *
 * Furi-free by design (host-testable in isolation): only libc headers.
 * The store is a fixed-capacity ring of N24_CAP_MAX entries; once full,
 * adding a new entry silently overwrites (drops) the oldest one. Iteration
 * via nrf24_capture_get() is always oldest-first logical order, regardless
 * of the underlying wrap point.
 */

typedef enum {
    N24_RAW = 0,
    N24_MOUSEJACK,
    N24_KEYBOARD,
    N24_BAYANG,
    N24_SYMA,
    N24_ESB,
    N24_V2X2,
} Nrf24Proto;

typedef struct {
    uint8_t addr[5];
    uint8_t channel;
    uint8_t payload[32];
    uint8_t len;
    uint32_t ts;
    uint8_t proto;
} Nrf24Capture;

#define N24_CAP_MAX 64

/* Clear the store (count -> 0). */
void nrf24_capture_reset(void);

/* Copy `c` into the store. The ~3 KB ring is allocated on the first add (not
 * resident at boot). If the store is at N24_CAP_MAX, the oldest entry is
 * dropped to make room. Returns true if stored; false if `c` is NULL or the
 * one-time ring allocation failed (out of heap -> the frame is dropped). */
bool nrf24_capture_add(const Nrf24Capture* c);

/* Number of entries currently stored, saturates at N24_CAP_MAX. */
size_t nrf24_capture_count(void);

/* 0-based, oldest-first. NULL if i >= nrf24_capture_count() (or never allocated). */
const Nrf24Capture* nrf24_capture_get(size_t i);

/* Free the on-demand ring back to the heap and reset the store to empty.
 * Call only after all capture writers (the promiscuous RX workers) have
 * stopped -- e.g. from nrf24_app_free, after every scene's on_exit. Safe to
 * call when nothing was ever captured (no-op). */
void nrf24_capture_free(void);

/* ---- Transmit / replay ----
 * Implemented in nrf24_capture_tx.c (NOT nrf24_capture.c) so this header's
 * store + its host test stay furi-free: these two pull in nrf24_hw.h (furi/
 * ESP-IDF SPI access) at the .c level. The declarations here only need the
 * libc types already in scope, so the header itself still has zero furi
 * dependency. */

/* Configure the radio for TX to `addr`/`chan` (5-byte ESB address, no AutoAck,
 * no retries -- mirrors nrf24_mj_core.c's mj_setup_tx_for_target()) and burst
 * `payload` (`len` bytes, clamped to 32) `repeats` times (repeats==0 treated
 * as 1). Self-contained: acquires/releases the radio internally, safe to call
 * without an outer nrf24_hw_acquire(). */
void nrf24_tx_raw(
    const uint8_t addr[5],
    uint8_t chan,
    const uint8_t* payload,
    uint8_t len,
    uint8_t repeats);
