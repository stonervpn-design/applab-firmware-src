#pragma once

// Generic car-fob button-table provider.
//
// Single source of truth mapping a SubGhz car protocol name string to the set
// of buttons that protocol's own encoder can synthesize (button code + label).
// Backed by per-protocol tables that reuse each protocol's real button-name map
// (see the audit block in subghz_fob_buttons.c). The multi-button rolling fob
// view (views/subghz_keeloq_fob) builds its tile grid from these tables so VAG,
// generic KeeLoq and every other supported car protocol share one render/nav/TX
// path.
//
// Deliberately self-contained (only <stdint.h>/<stddef.h>/<stdbool.h>) so it
// host-compiles under gcc without any furi/app headers (see
// tests/test_subghz_fob_buttons.c).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t code; // button code as written to the "Btn" fff field
    const char* name; // short label for the tile
} SubGhzFobButton;

// Return the button table for a protocol name (exact match on the protocol's
// own *_PROTOCOL_NAME string), or NULL if the protocol has no synthesizable,
// enumerable button set. On a hit *out_n gets the tile count; on a miss it is
// set to 0. out_n may be NULL.
const SubGhzFobButton* subghz_fob_buttons(const char* protocol_name, size_t* out_n);

// True iff protocol_name has an encoder that reads an independent button field
// AND an enumerable button set (i.e. subghz_fob_buttons returns a non-empty
// table). False for replay-only / unsupported protocols.
bool subghz_fob_protocol_supported(const char* protocol_name);

#ifdef __cplusplus
}
#endif
