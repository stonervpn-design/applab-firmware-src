#pragma once
/**
 * @file uart_monitor_fmt.h
 * Dependency-free capture ring + hex/ASCII line formatter for the on-screen
 * UART serial monitor scene. Host-testable (no furi/esp includes).
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define UART_RING_CAP 1024 /* fixed, static, bounded (no PSRAM) */

typedef struct {
    uint8_t data[UART_RING_CAP];
    uint16_t head; /* next write position */
    uint16_t len; /* valid bytes = min(pushed, UART_RING_CAP) */
} UartRing;

void uart_ring_reset(UartRing* r);

/** Push a byte; when full, the oldest byte is overwritten (drop-oldest). */
void uart_ring_push(UartRing* r, uint8_t byte);

/** Number of valid bytes currently held. */
uint16_t uart_ring_len(const UartRing* r);

/** Byte at logical index: 0 = oldest, len-1 = newest. Out of range -> 0x00. */
uint8_t uart_ring_get(const UartRing* r, uint16_t idx);

/**
 * Format up to 16 bytes as "XX XX .. |ascii|" (non-printable -> '.').
 * Always NUL-terminates and never writes past out_sz. Returns chars written
 * (excluding NUL), or 0 if args are invalid or out_sz < 40.
 */
size_t uart_fmt_hexascii_line(const uint8_t* bytes, uint8_t n, char* out, size_t out_sz);
