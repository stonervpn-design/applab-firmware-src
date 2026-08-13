#pragma once

/* DuckyScript OS-conditional block state machine.
 *
 * Pure, furi-free leaf: implements IF_OS/ELSE_OS/END_IF/SET_OS block
 * skipping for an operator-selected target OS. Host-compiles standalone
 * with only <stdint.h>/<stdbool.h>.
 *
 * Whole .c implementation is gated `#ifndef PUBLIC_BUILD` — bad_usb ships
 * in PUBLIC builds, but this feature is PRIVATE-only.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DUCKY_OS_WINDOWS = 0, DUCKY_OS_MAC = 1, DUCKY_OS_LINUX = 2 } DuckyOs;

typedef enum {
    DUCKY_COND_EXECUTE, /* run this (non-directive) line normally */
    DUCKY_COND_SKIP, /* inside a non-matching branch: no-op this line */
    DUCKY_COND_DIRECTIVE, /* line was IF_OS/ELSE_OS/END_IF/SET_OS, consumed */
    DUCKY_COND_ERROR /* malformed directive / unbalanced / overflow */
} DuckyCondResult;

#define DUCKY_OS_COND_MAX_DEPTH 8

typedef struct {
    DuckyOs target_os;
    uint8_t depth; /* open IF_OS blocks */
    bool execute[DUCKY_OS_COND_MAX_DEPTH]; /* per frame: run current branch? */
    bool matched[DUCKY_OS_COND_MAX_DEPTH]; /* IF_OS condition matched target */
    bool in_else[DUCKY_OS_COND_MAX_DEPTH]; /* currently in the ELSE_OS branch */
} DuckyOsCondState;

/* Reset state to no open blocks with the given initial target OS. */
void ducky_os_cond_reset(DuckyOsCondState* st, DuckyOs initial_os);

/* Evaluate one raw script line against the state machine. Updates state
 * for directive lines (IF_OS/ELSE_OS/END_IF/SET_OS). Never crashes on
 * malformed input; returns DUCKY_COND_ERROR instead. */
DuckyCondResult ducky_os_cond_eval(DuckyOsCondState* st, const char* line);

/* True if the top-of-stack frame is not currently executing (i.e. the
 * caller should treat non-directive lines as no-ops). */
bool ducky_os_cond_skipping(const DuckyOsCondState* st);

#ifdef __cplusplus
}
#endif
