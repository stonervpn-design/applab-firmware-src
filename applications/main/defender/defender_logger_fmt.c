#include "defender_logger_fmt.h"
#include <string.h>

/**
 * Pure format function: builds CSV row with no firmware dependencies.
 * Only depends on: stdint.h, stdbool.h, string.h, stdio.h
 */
size_t defender_logger_format_row(char* out, size_t out_sz, const DefenderAlert* a) {
    if(out_sz == 0) return 0;

    /* Build row in a scratch buffer first to measure length */
    char scratch[256];
    size_t pos = 0;

    /* Check if offender is all-zero */
    bool has_offender = false;
    for(int i = 0; i < 6; i++) {
        if(a->offender[i] != 0) {
            has_offender = true;
            break;
        }
    }

    /* Write timestamp, category, severity, and offender MAC */
    int n = snprintf(
        scratch, sizeof(scratch),
        "%lu,%s,%u,",
        (unsigned long)a->ts_ms,
        defender_cat_label(a->cat),
        (unsigned)a->severity);

    if(n < 0 || n >= (int)sizeof(scratch)) return 0;
    pos = (size_t)n;

    /* Write offender MAC (or empty string) */
    if(has_offender) {
        n = snprintf(
            scratch + pos, sizeof(scratch) - pos,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            a->offender[0], a->offender[1], a->offender[2],
            a->offender[3], a->offender[4], a->offender[5]);
    } else {
        n = 0;  /* Empty string */
    }

    if(n < 0 || pos + (size_t)n >= sizeof(scratch)) return 0;
    pos += (size_t)n;

    /* Write RSSI */
    n = snprintf(scratch + pos, sizeof(scratch) - pos, ",%d,", (int)a->rssi);
    if(n < 0 || pos + (size_t)n >= sizeof(scratch)) return 0;
    pos += (size_t)n;

    /* Determine if message needs escaping */
    bool needs_escape = false;
    for(size_t i = 0; a->msg[i]; i++) {
        if(a->msg[i] == ',' || a->msg[i] == '"' || a->msg[i] == '\n') {
            needs_escape = true;
            break;
        }
    }

    /* Write message */
    if(needs_escape) {
        if(pos + 1 >= sizeof(scratch)) return 0;
        scratch[pos++] = '"';

        /* Escape: double any quotes and copy message */
        for(size_t i = 0; a->msg[i]; i++) {
            if(a->msg[i] == '"') {
                if(pos + 2 >= sizeof(scratch)) return 0;
                scratch[pos++] = '"';
                scratch[pos++] = '"';
            } else {
                if(pos + 1 >= sizeof(scratch)) return 0;
                scratch[pos++] = a->msg[i];
            }
        }

        if(pos + 1 >= sizeof(scratch)) return 0;
        scratch[pos++] = '"';
    } else {
        /* No escaping needed, just copy message */
        if(pos + strlen(a->msg) + 1 >= sizeof(scratch)) return 0;
        strcpy(scratch + pos, a->msg);
        pos += strlen(a->msg);
    }

    /* Write newline */
    if(pos + 2 >= sizeof(scratch)) return 0;
    scratch[pos++] = '\n';
    scratch[pos] = '\0';

    /* Check if it fits in out[] */
    if(pos >= out_sz) return 0;

    /* Copy to output */
    strcpy(out, scratch);
    return pos;
}
