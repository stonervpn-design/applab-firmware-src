#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "defender_alert.h"

/**
 * Opaque logger structure (implementation detail).
 */
typedef struct DefenderLogger DefenderLogger;

/**
 * Open the SD event logger.
 * Creates /ext/apps_data/defender/events-<YYYYMMDD-HHMMSS>.csv with header row.
 * Returns NULL on SD failure (logging becomes a no-op).
 */
DefenderLogger* defender_logger_open(void);

/**
 * Close the logger and free resources.
 */
void defender_logger_close(DefenderLogger* logger);

/**
 * Log an alert: format row and append to file.
 * CSV row: ts_ms,category,severity,offender_mac,rssi,message
 * Flushes to SD every 8 rows.
 */
void defender_logger_log_alert(DefenderLogger* logger, const DefenderAlert* a);

/**
 * Phase 1 STUBS (implemented in Phase 2):
 */

/**
 * Begin a PCAP recording session with tag and max_packets limit.
 * Returns true on success, false on failure.
 */
bool defender_logger_pcap_begin(DefenderLogger* logger, const char* tag, uint32_t max_packets);

/**
 * Write a captured frame to the PCAP file.
 * ts_us: timestamp in microseconds.
 */
void defender_logger_pcap_write(DefenderLogger* logger, const uint8_t* frame, uint16_t len, uint32_t ts_us);

/**
 * End the PCAP recording session.
 */
void defender_logger_pcap_end(DefenderLogger* logger);
