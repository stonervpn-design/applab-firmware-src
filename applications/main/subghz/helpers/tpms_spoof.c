#include "tpms_spoof.h"

#ifndef PUBLIC_BUILD

#include "../subghz_i.h"
#include <lib/subghz/protocols/schrader_gg4.h>
#include <lib/subghz/protocols/tpms_generic.h>
#include <lib/subghz/types.h>

#define TAG "TpmsSpoof"

// Temp .sub the synthetic frame is written to before subghz_key_load picks it up.
#define TPMS_SPOOF_TMP_PATH EXT_PATH("subghz/.tpms_spoof.sub")
// 2-FSK, ~47.6 kHz deviation — the standard TPMS modulation; the .sub loader
// resolves this name to CC1101 registers.
#define TPMS_SPOOF_PRESET_NAME "FuriHalSubGhzPreset2FSKDev238Async"

// Only protocols with a field->data packer can be spoofed from scratch. Today
// that is Schrader GG4 (tpms_protocol_schrader_gg4_pack). Ford/Toyota/Renault/
// Citroen need packers written before they can be added here.
static const TpmsSpoofProto k_protos[] = {
    {
        .label = "Schrader GG4",
        .protocol_name = TPMS_PROTOCOL_SCHRADER_GG4_NAME,
        .preset_name = TPMS_SPOOF_PRESET_NAME,
        .frequency = 433920000, // EU; 315 MHz (US) is a documented alt
        .default_id = 0x0CA1B2C3,
        .default_pressure_bar = 2.20f, // ≈ 32 psi
        .default_temperature_c = 20.0f,
    },
};

size_t tpms_spoof_proto_count(void) {
    return COUNT_OF(k_protos);
}

const TpmsSpoofProto* tpms_spoof_proto_get(size_t index) {
    return index < COUNT_OF(k_protos) ? &k_protos[index] : NULL;
}

bool tpms_spoof_seed(
    SubGhz* subghz,
    size_t index,
    uint32_t id,
    float pressure_bar,
    float temperature_c,
    uint32_t frequency) {
    const TpmsSpoofProto* p = tpms_spoof_proto_get(index);
    if(!p) return false;

    // Build the frame from the operator's field values, then pack -> raw 64-bit
    // data + CRC.
    TPMSBlockGeneric g = {0};
    g.protocol_name = p->protocol_name;
    g.id = id;
    g.pressure = pressure_bar;
    g.temperature = temperature_c;
    g.data = 0; // status byte 0x00 == normal mode
    // (Schrader-only for now — extend when more packers land.)
    tpms_protocol_schrader_gg4_pack(&g); // sets g.data + g.data_count_bit = 64

    uint8_t data_be[8];
    for(int i = 0; i < 8; i++) data_be[i] = (uint8_t)(g.data >> (56 - i * 8));

    // Build a standard SubGhz key .sub by hand (tpms_block_generic_serialize uses
    // a TPMS-specific header that subghz_key_load rejects). key_load then loads the
    // Schrader decoder + this frame into the radio for the Transmitter to send.
    bool ok = false;
    FlipperFormat* ff = flipper_format_string_alloc();
    do {
        if(!flipper_format_write_header_cstr(ff, SUBGHZ_KEY_FILE_TYPE, SUBGHZ_KEY_FILE_VERSION))
            break;
        uint32_t freq = frequency, bit = g.data_count_bit, id_out = g.id, batt = 0;
        if(!flipper_format_write_uint32(ff, "Frequency", &freq, 1)) break;
        if(!flipper_format_write_string_cstr(ff, "Preset", p->preset_name)) break;
        if(!flipper_format_write_string_cstr(ff, "Protocol", p->protocol_name)) break;
        if(!flipper_format_write_uint32(ff, "Id", &id_out, 1)) break;
        if(!flipper_format_write_uint32(ff, "Bit", &bit, 1)) break;
        if(!flipper_format_write_hex(ff, "Data", data_be, sizeof(data_be))) break;
        if(!flipper_format_write_uint32(ff, "Batt", &batt, 1)) break;

        ok = subghz_save_protocol_to_file(subghz, ff, TPMS_SPOOF_TMP_PATH) &&
             subghz_key_load(subghz, TPMS_SPOOF_TMP_PATH, false);
    } while(false);

    if(!ok) FURI_LOG_E(TAG, "seed failed for %s", p->protocol_name);
    flipper_format_free(ff);
    return ok;
}

#endif // PUBLIC_BUILD
