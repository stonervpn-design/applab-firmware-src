#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "net_ntlm.h" // NtlmV2Parsed + net_ntlm_hashcat_5500()

/**
 * net_csv — append-mode CSV logging to the SD card via Furi storage
 * (firmware-only).
 *
 * net_csv_open() ensures the capture directory /ext/lan_attack/captures/ exists
 * (creating parent directories as needed) and opens the given file for append.
 * Writes are line-buffered: each write_line() appends the line plus '\n' and
 * flushes to the card so captures survive an unexpected power loss.
 */

typedef struct NetCsvLog NetCsvLog; // opaque

/**
 * Open (or create) the CSV file at path for appending. The parent capture
 * directory tree is created if missing. Returns an open NetCsvLog* on success,
 * NULL on error (logging then becomes a no-op the caller can ignore).
 */
NetCsvLog* net_csv_open(const char* path);

/**
 * Append csv_line followed by a newline, then flush to the card. NULL-safe on
 * both log and line (a NULL log is a silent no-op).
 */
void net_csv_write_line(NetCsvLog* log, const char* csv_line);

/** Flush, close and free the log. NULL-safe. */
void net_csv_close(NetCsvLog* log);

/**
 * Convenience writer: format the captured NetNTLMv2 exchange for hashcat mode
 * 5500 (reusing net_ntlm_hashcat_5500()) and append it as one CSV line.
 * NULL-safe on both arguments.
 */
void net_csv_write_ntlmv2_hashcat(NetCsvLog* log, const NtlmV2Parsed* parsed);
