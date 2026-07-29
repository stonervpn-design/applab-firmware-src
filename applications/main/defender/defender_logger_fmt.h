#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "defender_alert.h"

/**
 * Format a single CSV row for a DefenderAlert.
 *
 * Writes exactly one CSV row (with trailing '\n') for alert `a` into out[0..out_sz).
 * Columns: ts_ms,category,severity,offender_mac,rssi,message
 *
 * offender_mac format: "AA:BB:CC:DD:EE:FF" or "" if all-zero.
 * message escaping: if it contains ',', '"', or '\n' it is wrapped in double-quotes
 * and inner '"' are doubled ("" in CSV).
 *
 * Returns the number of bytes written (excluding NUL), or 0 if it would overflow out_sz.
 *
 * PURE: no side effects except writing to out[].
 */
size_t defender_logger_format_row(char* out, size_t out_sz, const DefenderAlert* a);
