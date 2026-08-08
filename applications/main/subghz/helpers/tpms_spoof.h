#pragma once
// From-scratch TPMS spoofer (PRIVATE-only). Seeds a synthetic TPMS sensor frame
// (ID + pressure + temperature) for a chosen protocol and hands it to the
// existing TPMS info view for edit + hold-OK burst TX. Only protocols with a
// field->data PACKER can be spoofed from scratch; today that is Schrader GG4.
// Ford/Toyota/Renault/Citroen need packers (added later) and are omitted here.
#ifndef PUBLIC_BUILD

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct SubGhz SubGhz;

// One spoofable protocol: menu label + the synthetic defaults + radio config.
typedef struct {
    const char* label; // submenu text
    const char* protocol_name; // must match the decoder's *_NAME
    const char* preset_name; // FuriHalSubGhzPreset* the .sub loader resolves
    uint32_t frequency; // Hz (433.92 EU default; 315 US is a documented alt)
    uint32_t default_id; // 32-bit sensor id
    float default_pressure_bar; // ~2.2 bar ≈ 32 psi
    float default_temperature_c;
} TpmsSpoofProto;

// Pure: number of spoofable protocols + the table entry (host-testable, no furi).
size_t tpms_spoof_proto_count(void);
const TpmsSpoofProto* tpms_spoof_proto_get(size_t index); // NULL if out of range

// Seed a synthetic frame for proto `index` from explicit field values: run the
// protocol packer (id/pressure/temp -> data + CRC), write a standard SubGhz key
// .sub by hand, and load it into the radio for the Transmitter to send. Returns
// true on success. (Implemented in tpms_spoof.c; needs furi.)
bool tpms_spoof_seed(
    SubGhz* subghz,
    size_t index,
    uint32_t id,
    float pressure_bar,
    float temperature_c,
    uint32_t frequency);

#endif // PUBLIC_BUILD
