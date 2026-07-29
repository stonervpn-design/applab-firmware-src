#include "nmea.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

void nmea_fix_init(GnssFix* fix) {
    memset(fix, 0, sizeof(*fix));
}

// Verify the "*HH" checksum if present. Returns true if absent or matching.
static bool nmea_checksum_ok(const char* line) {
    if(line[0] != '$') return false;
    const char* star = strchr(line, '*');
    uint8_t sum = 0;
    for(const char* p = line + 1; *p && p != star; p++) sum ^= (uint8_t)*p;
    if(!star) return true; // no checksum field — accept
    // two hex digits after '*'
    char hex[3] = {star[1], star[1] ? star[2] : 0, 0};
    if(!hex[0] || !hex[1]) return false;
    uint8_t want = (uint8_t)strtol(hex, NULL, 16);
    return want == sum;
}

// Split "line" (a copy) into up to max fields on ','; returns count. Trims the
// checksum: the field containing '*' is cut at '*'.
static int nmea_split(char* buf, char** fields, int max) {
    int n = 0;
    char* p = buf;
    fields[n++] = p;
    while(*p && n < max) {
        if(*p == ',') {
            *p = 0;
            fields[n++] = p + 1;
        } else if(*p == '*') {
            *p = 0; // drop checksum from the last field
            break;
        }
        p++;
    }
    return n;
}

// ddmm.mmmm + hemisphere -> signed decimal degrees. Empty -> 0.
static double nmea_coord(const char* val, const char* hemi) {
    if(!val || !val[0]) return 0;
    double raw = atof(val);
    // Reject NaN/inf or an out-of-range ddmm.mmmm magnitude (|value| <= 18000.0
    // for 180 deg). A garbled UART field could otherwise atof() to a huge value,
    // making the (int) cast below undefined behavior and producing a coordinate
    // that overflows the fixed CSV row buffer in wd_log_ap.
    if(!isfinite(raw) || raw < -18000.0 || raw > 18000.0) return 0;
    int deg = (int)(raw / 100.0);
    double minutes = raw - deg * 100.0;
    double dd = deg + minutes / 60.0;
    if(hemi && (hemi[0] == 'S' || hemi[0] == 'W')) dd = -dd;
    return dd;
}

// "hhmmss.ss" -> h/m/s
static void nmea_time(const char* t, uint8_t* h, uint8_t* m, uint8_t* s) {
    if(!t || strlen(t) < 6) return;
    char b[3] = {0};
    b[0] = t[0]; b[1] = t[1]; *h = (uint8_t)atoi(b);
    b[0] = t[2]; b[1] = t[3]; *m = (uint8_t)atoi(b);
    b[0] = t[4]; b[1] = t[5]; *s = (uint8_t)atoi(b);
}

bool nmea_parse_line(GnssFix* fix, const char* line) {
    if(!line || line[0] != '$') return false;
    if(!nmea_checksum_ok(line)) return false;

    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char* f[24];
    int n = nmea_split(buf, f, 24);
    if(n < 1) return false;

    const char* type = f[0] + 3; // skip "$GP"/"$GN"/... talker id (3 chars)

    if(strncmp(type, "GGA", 3) == 0 && n >= 10) {
        // $xxGGA,time,lat,N,lon,E,fixQual,numSats,hdop,alt,M,...
        int qual = f[6][0] ? atoi(f[6]) : 0;
        nmea_time(f[1], &fix->hour, &fix->minute, &fix->second);
        if(qual > 0) {
            fix->lat = nmea_coord(f[2], f[3]);
            fix->lon = nmea_coord(f[4], f[5]);
            fix->satellites = (uint8_t)(f[7][0] ? atoi(f[7]) : 0);
            fix->hdop_x10 = (uint16_t)(f[8][0] ? (atof(f[8]) * 10.0) : 0);
            fix->altitude_m = f[9][0] ? atof(f[9]) : 0;
            fix->valid = true;
        } else {
            fix->valid = false;
        }
        return true;
    }

    if(strncmp(type, "RMC", 3) == 0 && n >= 10) {
        // $xxRMC,time,status,lat,N,lon,E,speedKn,course,date,...
        nmea_time(f[1], &fix->hour, &fix->minute, &fix->second);
        bool active = (f[2][0] == 'A');
        if(active) {
            fix->lat = nmea_coord(f[3], f[4]);
            fix->lon = nmea_coord(f[5], f[6]);
            fix->speed_kmh = (f[7][0] ? atof(f[7]) : 0) * 1.852; // knots -> km/h
            fix->valid = true;
        } else {
            fix->valid = false;
        }
        // date ddmmyy in f[9]
        if(strlen(f[9]) >= 6) {
            char b[3] = {0};
            b[0] = f[9][0]; b[1] = f[9][1]; fix->day = (uint8_t)atoi(b);
            b[0] = f[9][2]; b[1] = f[9][3]; fix->month = (uint8_t)atoi(b);
            b[0] = f[9][4]; b[1] = f[9][5]; fix->year = (uint16_t)(2000 + atoi(b));
        }
        return true;
    }

    return false;
}
