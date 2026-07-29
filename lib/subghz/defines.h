#pragma once

// ProtoPirate automotive-decoder feature flags (ported into the MOB firmware).
// Kept minimal: enable emulation for the ported protocols. ProtoPirate's
// REMOVE_LOGS block is intentionally omitted so it does not silence FURI_LOG
// across the rest of the SubGhz app (which includes this transitively via
// protocol_items.h).
#define ENABLE_EMULATE_FEATURE

// ProtoPirate decoder/encoder feature flags. fiat_v1.c (and other ported
// ProtoPirate protocols) gate decoder/encoder declarations behind these; the .c
// bodies expect both enabled (the older ports hard-wire the decoder, and the
// registry references the encoder). NEW_v2's defines.h omitted them, which
// compiles the fiat_v1 decoder/encoder out and breaks registration.
#define PROTOPIRATE_WITH_DECODER 1
#define PROTOPIRATE_WITH_ENCODER 1
