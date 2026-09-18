#include "tpms_kia.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSKia"

// Port of rtl_433/src/devices/tpms_kia.c (Benjamin Larsson 2016, GPL-2+)
//
// FSK, Manchester encoded (FSK_PULSE_PCM, 50 us bit cells), seen on Kia Rio and
// other Hyundai/Kia sensors.  315 MHz (US) / 433.92 MHz (EU).
//
// On-air frame (154 raw bits):
//   [16-bit raw preamble 0xED71] [138 Manchester bits -> 69 data bits]
// The preamble is NOT Manchester coded; only the 138 bits after it are.
//
// Wire format (after Manchester decode, 9 bytes, b[8] holds only 5 top bits):
//   b[0]     : bits 7..4 = unknown (0xF), bits 3..0 = pressure high nibble
//   b[1]     : bits 7..4 = pressure low nibble, bits 3..0 = temperature high nibble
//   b[2]     : bits 7..4 = temperature low nibble, bits 3..0 = id[31..28]
//   b[3..5]  : id[27..4]
//   b[6]     : bits 7..4 = id[3..0], bits 3..0 = unknown2 high nibble
//   b[7]     : unknown2 low byte
//   b[8]     : bits 7..3 = CRC-8, bits 2..0 = forced 0
//
//   pressure = ((b[0] << 4) | (b[1] >> 4))       -> PSI = raw / 5.0
//   temp     = ((b[1] << 4) | (b[2] >> 4))       -> C   = raw - 50
//   id       = b[2]<<28 | b[3]<<20 | b[4]<<12 | b[5]<<4 | b[6]>>4
//   CRC-8 poly 0x07 init 0x76 over b[0..7]; valid when (b[8] & ~0x7) == crc8.
//
// Manchester polarity (rtl_433 bitbuffer_manchester_decode): for each raw pair
// (bit1,bit2) the decoded bit is bit2, and the pair is only valid when
// bit1 != bit2.

#define KIA_PREAMBLE      0xED71U
#define KIA_PREAMBLE_MASK 0xFFFFU
#define KIA_DATA_BITS     64 // stored/serialized portion (b[0..7])
#define KIA_DECODED_BITS  69 // decoded Manchester bits needed for a full frame

static const SubGhzBlockConst tpms_protocol_kia_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25,
    .min_count_bit_for_found = KIA_DATA_BITS,
};

typedef enum {
    KiaDecoderStepFindPreamble = 0,
    KiaDecoderStepData,
} KiaDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint32_t preamble_reg; // raw grid-bit history for preamble search
    uint8_t packet[9]; // decoded frame bytes
    uint16_t decoded_bits; // count of Manchester-decoded bits collected
    uint8_t man_first; // first raw half-bit of a pending Manchester pair
    bool man_have_first;
} TPMSProtocolDecoderKia;

const SubGhzProtocolDecoder tpms_protocol_kia_decoder = {
    .alloc = tpms_protocol_decoder_kia_alloc,
    .free = tpms_protocol_decoder_kia_free,
    .feed = tpms_protocol_decoder_kia_feed,
    .reset = tpms_protocol_decoder_kia_reset,
    .get_hash_data = tpms_protocol_decoder_kia_get_hash_data,
    .serialize = tpms_protocol_decoder_kia_serialize,
    .deserialize = tpms_protocol_decoder_kia_deserialize,
    .get_string = tpms_protocol_decoder_kia_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_kia_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_kia = {
    .name = TPMS_PROTOCOL_KIA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_kia_decoder,
    .encoder = &tpms_protocol_kia_encoder,
};

void* tpms_protocol_decoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderKia* instance = malloc(sizeof(TPMSProtocolDecoderKia));
    instance->base.protocol = &tpms_protocol_kia;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_kia_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    free(instance);
}

void tpms_protocol_decoder_kia_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    instance->decoder.parser_step = KiaDecoderStepFindPreamble;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_reg = 0;
    instance->decoded_bits = 0;
    instance->man_have_first = false;
}

// CRC-8, MSB-first, non-reflected (matches rtl_433 crc8()).
static uint8_t kia_crc8(const uint8_t* data, size_t len, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            if(crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ poly);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void tpms_protocol_kia_analyze(TPMSProtocolDecoderKia* instance) {
    const uint8_t* b = instance->packet;

    uint16_t pressure_raw = (uint16_t)(((b[0] << 4) | (b[1] >> 4)) & 0xFF);
    uint16_t temp_raw = (uint16_t)(((b[1] << 4) | (b[2] >> 4)) & 0xFF);
    uint32_t id = ((uint32_t)(b[2] & 0x0F) << 28) | ((uint32_t)b[3] << 20) |
                  ((uint32_t)b[4] << 12) | ((uint32_t)b[5] << 4) | (uint32_t)(b[6] >> 4);

    instance->generic.id = id;
    // rtl_433 reports PSI (raw / 5.0); generic.pressure is stored in bar.
    float psi = (float)pressure_raw / 5.0f;
    instance->generic.pressure = psi * 0.0689476f;
    instance->generic.temperature = (float)((int)temp_raw - 50);
    instance->generic.battery_low = TPMS_NO_BATT;

    // Store b[0..7] big-endian for serialize/hash/display reconstruction.
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    instance->generic.data = data;
    instance->generic.data_count_bit = KIA_DATA_BITS;
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = KIA_DATA_BITS;
}

static void tpms_protocol_kia_finalize(TPMSProtocolDecoderKia* instance) {
    // Reject frames whose CRC does not check out (prevents false positives).
    uint8_t crc = kia_crc8(instance->packet, 8, 0x07, 0x76);
    if((instance->packet[8] & (uint8_t)(~0x7)) != crc) return;

    tpms_protocol_kia_analyze(instance);
    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
}

// Process a single raw (PCM grid) bit through the preamble/Manchester machine.
static void tpms_protocol_kia_grid_bit(TPMSProtocolDecoderKia* instance, uint8_t level) {
    if(instance->decoder.parser_step == KiaDecoderStepData) {
        if(!instance->man_have_first) {
            instance->man_first = level;
            instance->man_have_first = true;
            return;
        }
        instance->man_have_first = false;
        if(instance->man_first == level) {
            // Manchester violation: drop back to preamble search, reusing this
            // bit as a preamble candidate below.
            instance->decoder.parser_step = KiaDecoderStepFindPreamble;
            instance->decoded_bits = 0;
        } else {
            // Decoded bit == second half-bit (rtl_433: 01->0, 10->1).
            uint8_t decoded = level;
            uint16_t idx = instance->decoded_bits;
            if(decoded) instance->packet[idx >> 3] |= (uint8_t)(1u << (7 - (idx & 7)));
            instance->decoded_bits++;
            if(instance->decoded_bits >= KIA_DECODED_BITS) {
                tpms_protocol_kia_finalize(instance);
                instance->decoder.parser_step = KiaDecoderStepFindPreamble;
                instance->decoded_bits = 0;
                instance->man_have_first = false;
            }
            return;
        }
    }

    // KiaDecoderStepFindPreamble
    instance->preamble_reg = (instance->preamble_reg << 1) | (level ? 1u : 0u);
    if((instance->preamble_reg & KIA_PREAMBLE_MASK) == KIA_PREAMBLE) {
        instance->decoder.parser_step = KiaDecoderStepData;
        instance->decoded_bits = 0;
        instance->man_have_first = false;
        for(size_t i = 0; i < sizeof(instance->packet); i++) instance->packet[i] = 0;
    }
}

void tpms_protocol_decoder_kia_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;

    // Slice this level into PCM grid cells of te_short (~50 us).
    uint32_t n = (duration + (tpms_protocol_kia_const.te_short / 2)) /
                 tpms_protocol_kia_const.te_short;

    if(n == 0 || n > 4) {
        // Noise or inter-frame gap: reset, discarding any partial frame.
        instance->decoder.parser_step = KiaDecoderStepFindPreamble;
        instance->preamble_reg = 0;
        instance->decoded_bits = 0;
        instance->man_have_first = false;
        return;
    }

    for(uint32_t i = 0; i < n; i++) {
        tpms_protocol_kia_grid_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_kia_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_kia_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_kia_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "n/a" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
