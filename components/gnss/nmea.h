#pragma once

// Minimal NMEA-0183 parser for the ATGM336H GNSS on the Cap LoRa-1262.
// Pure logic (no furi/ESP deps) so it is host-testable. Feed it complete
// sentences (one line, without the trailing CR/LF); it accumulates the latest
// fix. Handles GGA (fix quality, sats, altitude) and RMC (status, lat/lon, speed,
// date/time) from any talker id (GP/GN/GL/GA/BD/QZ...).

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid; // true once a positional fix is present (GGA quality>0 or RMC 'A')
    double lat; // decimal degrees, +N / -S
    double lon; // decimal degrees, +E / -W
    double altitude_m; // metres (GGA), 0 if unknown
    double speed_kmh; // over ground (RMC), 0 if unknown
    uint8_t satellites; // sats in use (GGA)
    uint16_t hdop_x10; // horizontal dilution * 10 (GGA), 0 if unknown
    // UTC, from GGA/RMC time + RMC date. Zero if not yet seen.
    uint8_t hour, minute, second;
    uint8_t day, month;
    uint16_t year; // full year, e.g. 2026
} GnssFix;

/** Reset a fix to empty/no-lock. */
void nmea_fix_init(GnssFix* fix);

/**
 * Parse ONE NMEA sentence into `fix`. Returns true if the sentence was a
 * recognized, well-formed GGA/RMC that updated the fix. The checksum (if present
 * after '*') is verified; a bad checksum is rejected.
 */
bool nmea_parse_line(GnssFix* fix, const char* line);
