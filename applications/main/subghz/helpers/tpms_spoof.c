#include "tpms_spoof.h"

#ifndef PUBLIC_BUILD

#include "../subghz_i.h"
#include <lib/subghz/protocols/schrader_gg4.h>
#include <lib/subghz/protocols/tpms_ford.h>
#include <lib/subghz/protocols/tpms_renault.h>
#include <lib/subghz/protocols/tpms_citroen.h>
#include <lib/subghz/protocols/tpms_pmv107j.h>
#include <lib/subghz/protocols/tpms_generic.h>
#include <lib/subghz/types.h>
#include <string.h>

#define TAG "TpmsSpoof"

// Temp .sub the synthetic frame is written to before subghz_key_load picks it up.
#define TPMS_SPOOF_TMP_PATH EXT_PATH("subghz/.tpms_spoof.sub")
// 2-FSK, ~47.6 kHz deviation — the standard TPMS modulation; the .sub loader
// resolves this name to CC1101 registers.
#define TPMS_SPOOF_PRESET_NAME "FuriHalSubGhzPreset2FSKDev238Async"

// Only protocols with a field->data packer can be spoofed from scratch. Each
// packer is the inverse of its decoder's analyze, and produces bytes that pass
// that decoder's check gate (CRC / checksum / integrity byte) — verified in
// tools/tpms_spoof/multi_pack_replica.py.
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
    {
        .label = "Ford TPMS",
        .protocol_name = TPMS_PROTOCOL_FORD_NAME,
        .preset_name = TPMS_SPOOF_PRESET_NAME,
        .frequency = 433920000, // EU; 315 MHz (US) common on Ford
        .default_id = 0x0089ABCD,
        .default_pressure_bar = 2.20f,
        .default_temperature_c = 20.0f,
    },
    {
        .label = "Renault TPMS",
        .protocol_name = TPMS_PROTOCOL_RENAULT_NAME,
        .preset_name = TPMS_SPOOF_PRESET_NAME,
        .frequency = 433920000, // EU
        .default_id = 0x00123456, // 24-bit sensor id
        .default_pressure_bar = 2.20f,
        .default_temperature_c = 20.0f,
    },
    {
        .label = "Citroen TPMS",
        .protocol_name = TPMS_PROTOCOL_CITROEN_NAME,
        .preset_name = TPMS_SPOOF_PRESET_NAME,
        .frequency = 433920000, // EU; PSA group (Citroen/Peugeot)
        .default_id = 0x00ABCDEF, // low 24 bits are free; top byte = XOR checksum
        .default_pressure_bar = 2.20f,
        .default_temperature_c = 20.0f,
    },
    {
        .label = "Toyota PMV-107J",
        .protocol_name = TPMS_PROTOCOL_PMV107J_NAME,
        .preset_name = TPMS_SPOOF_PRESET_NAME,
        .frequency = 433920000, // EU; 315 MHz (US/JP) common on Toyota/Lexus
        .default_id = 0x0123456, // 28-bit sensor id
        .default_pressure_bar = 2.20f,
        .default_temperature_c = 20.0f,
    },
};

// Dispatch the friendly-fields -> data+CRC packer by protocol. Only protocols
// with a packer can be spoofed from scratch.
static bool tpms_spoof_pack(const char* protocol_name, TPMSBlockGeneric* g) {
    if(strcmp(protocol_name, TPMS_PROTOCOL_SCHRADER_GG4_NAME) == 0) {
        tpms_protocol_schrader_gg4_pack(g);
        return true;
    }
    if(strcmp(protocol_name, TPMS_PROTOCOL_FORD_NAME) == 0) {
        tpms_protocol_ford_pack(g);
        return true;
    }
    if(strcmp(protocol_name, TPMS_PROTOCOL_RENAULT_NAME) == 0) {
        tpms_protocol_renault_pack(g);
        return true;
    }
    if(strcmp(protocol_name, TPMS_PROTOCOL_CITROEN_NAME) == 0) {
        tpms_protocol_citroen_pack(g);
        return true;
    }
    if(strcmp(protocol_name, TPMS_PROTOCOL_PMV107J_NAME) == 0) {
        tpms_protocol_pmv107j_pack(g);
        return true;
    }
    return false;
}

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
    if(!tpms_spoof_pack(p->protocol_name, &g)) {
        FURI_LOG_E(TAG, "no packer for %s", p->protocol_name);
        return false;
    }

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
