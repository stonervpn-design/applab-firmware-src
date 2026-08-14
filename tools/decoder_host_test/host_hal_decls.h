// Force-included (gcc -include) prototypes for the firmware furi_hal functions the
// lib/subghz automotive decoders call but whose real headers aren't on the host
// include path. Definitions live in host_shim.c. GCC 15 treats a missing
// prototype (implicit declaration) as a hard error, so declaring it here lets the
// decoder .c files compile unchanged.
#pragma once
#include <stdint.h>

// Real firmware default is 1 (see host_shim.c). Return type matches host_shim.c.
int32_t furi_hal_subghz_get_rolling_counter_mult(void);
