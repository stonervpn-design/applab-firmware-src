#pragma once
/* Host-test shim for <furi.h>.
 *
 * lib/subghz/protocols/keeloq_common.c and keeloq_common.h #include <furi.h>
 * but (verified by grep) never call furi_assert/furi_check/FURI_LOG_* or
 * reference any furi type -- the crypto is pure integer math. This stub
 * just needs to satisfy the #include so the REAL, unmodified
 * keeloq_common.c compiles; the macros below are defined anyway per the
 * task spec, in case that ever changes.
 *
 * The real furi.h transitively brings in stdint.h/stdbool.h (keeloq_common.h
 * uses uint32_t/uint64_t directly), so this stub must too.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define furi_assert(x) ((void)0)
#define furi_check(x)  ((void)0)
#define FURI_LOG_D(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
#define FURI_LOG_T(...) ((void)0)
