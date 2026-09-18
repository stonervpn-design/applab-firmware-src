#include "tpms_smartire.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSSmarTire"

// Port of rtl_433/src/devices/tpms_smartire.c (Bruno OCTAU / ProfBoc75 2024, GPL-2+)
//
// SmarTire Vantage / Aston Martin DB9 protocol (1/2005 - 12/2011).
// OOK PCM, Differential-Manchester coded, 167 us chip.
// 315 MHz (US) / 433.92 MHz (EU).
//
// A burst is 10 messages: a Pressure message then a Temperature message,
// each repeated 5 times, sent every 2 seconds.
//
// Wire format (after differential-Manchester decoding), 6 bytes / 48 bits:
//   VV MMII IIII IIII FFFFFFXX CC
//   b[0]        = Value: pressure (offset 40, *2.5 -> kPa) if type 0,
//                        temperature (offset 40) if type 1
//   b[1] bit7:6 = Message type (0 = pressure, 1 = temperature)
//   b[1] bit5:0 } 22-bit Sensor ID (b[1]&0x3f << 16 | b[2] << 8 | b[3])
//   b[2],b[3]   }
//   b[4] bit7   = Fast-inflate flag
//   b[4] bit6:0 = Flags (bit1 = quick inflate; low 2 bits look like parity)
//   b[5] bit7:1 = CRC-7 (poly 0x45, init 0x6f, final XOR 0x00, covers all 6 bytes)
//
// Preamble/sync word (raw PCM chips, NOT DM coded): 0x32b4 (16 bits).
// The DM decode always drops the final bit, so 47 decoded bits is a full frame.

#define SMARTIRE_PREAMBLE      0x32B4U
#define SMARTIRE_PREAMBLE_BITS 16
#define SMARTIRE_MSG_BYTES     6
#define SMARTIRE_DATA_BITS     48 // 6 bytes, stored (last chip bit is zero-filled)
#define SMARTIRE_MIN_DEC_BITS  47 // rtl_433: decoded row is always one bit short
#define SMARTIRE_MAX_CHIPS     128

static const SubGhzBlockConst tpms_protocol_smartire_const = {
    .te_short = 167, // one PCM chip
    .te_long = 334, // two chips (max run in valid data)
    .te_delta = 100,
    .min_count_bit_for_found = SMARTIRE_DATA_BITS,
};

typedef enum {
    SmartireDecoderStepReset = 0,
    SmartireDecoderStepFindPreamble,
    SmartireDecoderStepCollectData,
} SmartireDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint8_t chips[SMARTIRE_MAX_CHIPS];
    size_t chip_count;
} TPMSProtocolDecoderSmarTire;

const SubGhzProtocolDecoder tpms_protocol_smartire_decoder = {
    .alloc = tpms_protocol_decoder_smartire_alloc,
    .free = tpms_protocol_decoder_smartire_free,
    .feed = tpms_protocol_decoder_smartire_feed,
    .reset = tpms_protocol_decoder_smartire_reset,
    .get_hash_data = tpms_protocol_decoder_smartire_get_hash_data,
    .serialize = tpms_protocol_decoder_smartire_serialize,
    .deserialize = tpms_protocol_decoder_smartire_deserialize,
    .get_string = tpms_protocol_decoder_smartire_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_smartire_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_smartire = {
    .name = TPMS_PROTOCOL_SMARTIRE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_smartire_decoder,
    .encoder = &tpms_protocol_smartire_encoder,
};

void* tpms_protocol_decoder_smartire_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSmarTire* instance = malloc(sizeof(TPMSProtocolDecoderSmarTire));
    instance->base.protocol = &tpms_protocol_smartire;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_smartire_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    free(instance);
}

void tpms_protocol_decoder_smartire_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    instance->decoder.parser_step = SmartireDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->chip_count = 0;
}

// rtl_433 crc7(): 7-bit CRC working in the top bits of an 8-bit remainder.
static uint8_t tpms_protocol_smartire_crc7(const uint8_t* message, unsigned n_bytes) {
    unsigned remainder = 0x6F << 1; // init 0x6f, LSB unused
    unsigned poly = 0x45 << 1; // poly 0x45
    for(unsigned byte = 0; byte < n_bytes; ++byte) {
        remainder ^= message[byte];
        for(unsigned bit = 0; bit < 8; ++bit) {
            if(remainder & 0x80) {
                remainder = (remainder << 1) ^ poly;
            } else {
                remainder = (remainder << 1);
            }
        }
    }
    return (remainder >> 1) & 0x7F; // discard the unused LSB
}

// Faithful port of bitbuffer_differential_manchester_decode() over a raw
// chip array. Returns the number of decoded bits written to out[].
static size_t tpms_protocol_smartire_dm_decode(
    const uint8_t* chips,
    size_t chip_count,
    uint8_t* out,
    size_t max_bits) {
    size_t len = chip_count;
    if(max_bits && len > max_bits * 2) len = max_bits * 2;

    size_t ipos = 0;
    size_t obits = 0;
    uint8_t bit1;
    uint8_t bit2 = 0;

#define SMARTIRE_CHIP_AT(i) (((i) < chip_count) ? (chips[(i)] ? 1 : 0) : 0)

    // Sync loop: the first long pulse determines the clock; leading short
    // transitions are decoded as 0 bits until alignment is found.
    while(ipos < len) {
        bit1 = SMARTIRE_CHIP_AT(ipos);
        ipos++;
        bit2 = SMARTIRE_CHIP_AT(ipos);
        ipos++;
        uint8_t bit3 = SMARTIRE_CHIP_AT(ipos); // peek, no advance

        if(bit1 != bit2) {
            if(bit2 != bit3) {
                if(obits < max_bits) out[obits++] = 0;
            } else {
                bit2 = bit1;
                ipos -= 1;
                break;
            }
        } else {
            bit2 = 1 - bit1;
            ipos -= 2;
            break;
        }
    }

    // Main loop: each data bit is a pair of chips; the boundary between bits
    // must always carry a transition (the clock), else the frame is aborted.
    while(ipos < len) {
        bit1 = SMARTIRE_CHIP_AT(ipos);
        ipos++;
        if(bit1 == bit2) break; // clock missing, abort
        bit2 = SMARTIRE_CHIP_AT(ipos);
        ipos++;
        if(bit1 == bit2) {
            if(obits < max_bits) out[obits++] = 1;
        } else {
            if(obits < max_bits) out[obits++] = 0;
        }
    }

#undef SMARTIRE_CHIP_AT
    return obits;
}

static void tpms_protocol_smartire_analyze(TPMSBlockGeneric* g) {
    uint8_t b[SMARTIRE_MSG_BYTES];
    for(int i = 0; i < SMARTIRE_MSG_BYTES; i++) {
        b[i] = (g->data >> (8 * (SMARTIRE_MSG_BYTES - 1 - i))) & 0xFF;
    }

    g->id = (uint32_t)((b[1] & 0x3F) << 16) | ((uint32_t)b[2] << 8) | b[3];

    uint8_t msg_type = (b[1] & 0xC0) >> 6;
    int value = (int)b[0] - 40;

    if(msg_type == 0) {
        // Pressure: value * 2.5 kPa -> bar (kPa * 0.01)
        g->pressure = (float)value * 2.5f * 0.01f;
        g->temperature = -100.0f; // this frame carries no temperature
    } else {
        // Temperature message (msg_type == 1)
        g->temperature = (float)value;
        g->pressure = -1.0f; // this frame carries no pressure
    }

    // SmarTire frames do not report a battery state.
    g->battery_low = TPMS_NO_BATT;
}

// Decode the collected chip stream; on a valid CRC, fire the callback.
static void tpms_protocol_smartire_finalize(TPMSProtocolDecoderSmarTire* instance) {
    uint8_t bits[SMARTIRE_DATA_BITS];
    size_t nbits = tpms_protocol_smartire_dm_decode(
        instance->chips, instance->chip_count, bits, SMARTIRE_DATA_BITS);

    if(nbits < SMARTIRE_MIN_DEC_BITS) return;

    uint8_t b[SMARTIRE_MSG_BYTES] = {0};
    for(size_t i = 0; i < nbits && i < SMARTIRE_DATA_BITS; i++) {
        if(bits[i]) b[i / 8] |= (uint8_t)(1 << (7 - (i % 8)));
    }

    // CRC-7 over all 6 bytes must be zero (rejects false positives).
    if(tpms_protocol_smartire_crc7(b, SMARTIRE_MSG_BYTES) != 0) return;

    uint64_t data = 0;
    for(int i = 0; i < SMARTIRE_MSG_BYTES; i++) {
        data = (data << 8) | b[i];
    }

    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = SMARTIRE_DATA_BITS;
    instance->generic.data = data;
    instance->generic.data_count_bit = SMARTIRE_DATA_BITS;
    tpms_protocol_smartire_analyze(&instance->generic);

    if(instance->base.callback)
        instance->base.callback(&instance->base, instance->base.context);
}

void tpms_protocol_decoder_smartire_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;

    // Slice this level/duration into raw PCM chips (1 or 2 of the same level).
    uint8_t chip_run;
    if(DURATION_DIFF(duration, tpms_protocol_smartire_const.te_short) <
       tpms_protocol_smartire_const.te_delta) {
        chip_run = 1;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_smartire_const.te_long) <
        tpms_protocol_smartire_const.te_delta) {
        chip_run = 2;
    } else {
        // Out of tolerance: end of packet / noise. Try what we have, then reset.
        if(instance->decoder.parser_step == SmartireDecoderStepCollectData &&
           instance->chip_count > 0) {
            tpms_protocol_smartire_finalize(instance);
        }
        instance->decoder.parser_step = SmartireDecoderStepReset;
        instance->header_shift = 0;
        instance->chip_count = 0;
        return;
    }

    for(uint8_t c = 0; c < chip_run; c++) {
        uint8_t chip = level ? 1 : 0;

        switch(instance->decoder.parser_step) {
        case SmartireDecoderStepReset:
            instance->header_shift = 0;
            instance->chip_count = 0;
            instance->decoder.parser_step = SmartireDecoderStepFindPreamble;
            /* fallthrough */
        case SmartireDecoderStepFindPreamble:
            instance->header_shift =
                (uint16_t)((instance->header_shift << 1) | (chip ? 1 : 0));
            if(instance->header_shift == SMARTIRE_PREAMBLE) {
                instance->decoder.parser_step = SmartireDecoderStepCollectData;
                instance->chip_count = 0;
            }
            break;

        case SmartireDecoderStepCollectData:
            if(instance->chip_count < SMARTIRE_MAX_CHIPS) {
                instance->chips[instance->chip_count++] = chip;
            }
            if(instance->chip_count >= SMARTIRE_DATA_BITS * 2) {
                tpms_protocol_smartire_finalize(instance);
                instance->decoder.parser_step = SmartireDecoderStepReset;
                instance->header_shift = 0;
                instance->chip_count = 0;
            }
            break;
        }
    }
}

uint8_t tpms_protocol_decoder_smartire_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_smartire_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_smartire_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_smartire_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_smartire_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%d\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
