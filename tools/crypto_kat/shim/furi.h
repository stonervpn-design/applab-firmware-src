#pragma once
/* Host-test shim for <furi.h>.
 *
 * The staged crypto sources include furi.h via their headers but never call any
 * furi_assert / furi_check / FURI_LOG entry point and reference no furi type --
 * the crypto is pure integer math. This stub only satisfies the include so the
 * REAL, unmodified sources compile. The real furi.h transitively provides the
 * fixed-width int types AND string.h (aes_common.c uses memcpy without its own
 * include), so this stub pulls those in too.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define furi_assert(x) ((void)0)
#define furi_check(x)  ((void)0)
#define FURI_LOG_D(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
#define FURI_LOG_T(...) ((void)0)
