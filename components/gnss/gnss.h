#pragma once

// GNSS reader for the Cap LoRa-1262 (ATGM336H) on the Cardputer-ADV.
// UART @ 115200 8N1, host-RX = GPIO15, host-TX = GPIO13. Runs a background task
// that reads NMEA lines and keeps the latest fix. Thread-safe getter.

#include "nmea.h"
#include <stdbool.h>

#define GNSS_UART_RX_PIN 15
#define GNSS_UART_TX_PIN 13
#define GNSS_UART_BAUD   115200

/** Start the GNSS UART + reader task. Safe to call twice (no-op if running). */
bool gnss_start(void);

/** Stop the reader task and release the UART. */
void gnss_stop(void);

/** Is the reader running? */
bool gnss_is_running(void);

/** Copy the latest fix out (thread-safe). Returns fix->valid for convenience. */
bool gnss_get_fix(GnssFix* out);

/** Total NMEA lines seen since start (for a liveness indicator in the UI). */
uint32_t gnss_line_count(void);
