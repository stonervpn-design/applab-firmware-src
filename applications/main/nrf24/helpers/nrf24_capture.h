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

/* Copy `c` into the store. Always succeeds: if the store is at N24_CAP_MAX,
 * the oldest entry is dropped to make room. Returns true if stored (always,
 * for a non-NULL `c`; false only if `c` is NULL). */
bool nrf24_capture_add(const Nrf24Capture* c);

/* Number of entries currently stored, saturates at N24_CAP_MAX. */
size_t nrf24_capture_count(void);

/* 0-based, oldest-first. NULL if i >= nrf24_capture_count(). */
const Nrf24Capture* nrf24_capture_get(size_t i);
