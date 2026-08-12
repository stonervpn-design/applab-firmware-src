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

// The FlipperFormat field name each protocol's encoder deserialize reads for the
// button code (what the fob-emulate press path must write). Most protocols read
// "Btn"; the Honda families differ: V5/V7/V8/V9 read "Button", V2&V3 reads
// "BtnLo". Returns the matching field for a known protocol, or the "Btn" default
// for any other (never NULL) — see the audit block in subghz_fob_buttons.c.
const char* subghz_fob_button_field(const char* protocol_name);

// The FlipperFormat field each protocol's encoder reads for the rolling counter
// (what the fob-emulate press path must advance+persist between presses). Most
// protocols keep the counter in "Cnt" (pp_encoder_read_fields family); the
// SantaFe / Honda rolling families read it from a different field: SantaFe and
// Honda V2&V3 read "Rolling", Honda V7/V9 read "Counter", Honda V8 reads
// "RollingCode", Honda V5 reads "Counter". Returns the matching field for a
// known protocol, or the "Cnt" default for any other (never NULL) — see the
// per-protocol encoder deserialize referenced in the audit block.
const char* subghz_fob_counter_field(const char* protocol_name);

#ifdef __cplusplus
}
#endif
