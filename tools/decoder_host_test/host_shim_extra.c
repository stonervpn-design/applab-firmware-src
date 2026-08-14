// Extra host stubs for the FULL lib/subghz automotive harness (pp_decode_full).
// Kept SEPARATE from host_shim.c so the existing pp_decode build is untouched.
// Only missing HAL/platform symbols are stubbed here -- never decoder logic.
// Benign default return values (counter mult = 1, etc.).
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// (stubs added iteratively as the linker reports undefined references)
