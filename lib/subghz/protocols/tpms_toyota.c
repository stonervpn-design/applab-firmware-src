#include "tpms_toyota.h"

#define TAG "TPMSToyota"

// Port of rtl_433/src/devices/tpms_toyota.c (Christian W. Zuckschwerdt, GPL-2+).
//
// FSK, Differential-Manchester encoded, 9-byte payload with a CRC-8.
// Toyota (and Lexus/Scion) direct TPMS. 315 MHz (US/JP) / 433.92 MHz (EU).
//
// Wire format (after Differential-Manchester decoding), 9 bytes = 72 bits:
//
//   II II II II 4P 5T 6S NP CC
//
//   b[0..3] = 32-bit sensor ID
//   b[4]    = bit7 status MSB, bits[6:0] = pressure high 7 bits
//   b[5]    = bit7 pressure LSB, bits[6:0] = temperature high 7 bits
//   b[6]    = bit7 temperature LSB, bits[6:0] = 7-bit state/status field
//   b[7]    = inverted pressure (b[7] == pressure1 ^ 0xff) — integrity check
//   b[8]    = CRC-8, poly 0x07, init 0x80, over b[0..7]
//
//   pressure1 = ((b[4] & 0x7f) << 1) | (b[5] >> 7)   // 8-bit
//   temp      = ((b[5] & 0x7f) << 1) | (b[6] >> 7)   // 8-bit
//   status    = (b[4] & 0x80) | (b[6] & 0x7f)
//   pressure_PSI  = pressure1 * 0.25 - 7.0
//   temperature_C = temp - 40
//
// Encoding is the same Differential-Manchester scheme as the sibling
// Pacific/Toyota PMV-107J sensor (tpms_pmv107j.c), so this reuses that proven
// on-the-fly DM front-end with Toyota-matched half-bit timing (52 us, i.e. the
// rtl_433 tpms_toyota 52 us symbol; same as the Ford/Citroen ports):
//   - duration ~ te_long  → full bit period, no mid-bit transition → emit "1"
//   - duration ~ te_short → half-bit. First half only marks "saw start
//     transition"; the second half completes a mid-transition bit → emit "0".
//   State starts UNCLOCKED; the first te_long anchors the clock. Idle carrier
//   between packets decodes to a run of "1"s, so we sync on the decoded
//   6-bit preamble 0b111110 (0x3E) — five ones terminated by the first zero,
//   evaluated in a 6-bit sliding window (mask 0x3F) — then collect the 72 data
//   bits. Frames are gated on BOTH the exact CRC-8 AND the pressure1 ==
//   (b[7]^0xff) sanity check, so misaligned/false frames are rejected rather
//   than reported (decode-only).

#define TOYOTA_PREAMBLE 0x3EU // 0b111110 in a 6-bit window (0x3F mask)
#define TOYOTA_PREAMBLE_BITS 6
#define TOYOTA_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_toyota_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TOYOTA_DATA_BITS,
};

typedef enum {
    ToyotaDmUnclocked = 0, // not yet seen any te_long → no sync
    ToyotaDmAtBoundary, // about to start a new bit
    ToyotaDmAtMid, // saw mid-bit transition, waiting for the next-bit boundary
} ToyotaDmState;

typedef enum {
    ToyotaStepReset = 0,
    ToyotaStepFindPreamble,
    ToyotaStepData,
} ToyotaStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    ToyotaDmState dm_state;
    uint8_t preamble_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderToyota;

const SubGhzProtocolDecoder tpms_protocol_toyota_decoder = {
    .alloc = tpms_protocol_decoder_toyota_alloc,
    .free = tpms_protocol_decoder_toyota_free,
    .feed = tpms_protocol_decoder_toyota_feed,
    .reset = tpms_protocol_decoder_toyota_reset,
    .get_hash_data = tpms_protocol_decoder_toyota_get_hash_data,
    .serialize = tpms_protocol_decoder_toyota_serialize,
    .deserialize = tpms_protocol_decoder_toyota_deserialize,
    .get_string = tpms_protocol_decoder_toyota_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_toyota_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_toyota = {
    .name = TPMS_PROTOCOL_TOYOTA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_toyota_decoder,
    .encoder = &tpms_protocol_toyota_encoder,
};

void* tpms_protocol_decoder_toyota_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderToyota* instance = malloc(sizeof(TPMSProtocolDecoderToyota));
    instance->base.protocol = &tpms_protocol_toyota;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_toyota_free(void* context) {
    furi_assert(context);
    free(context);
}

static void toyota_reset_state(TPMSProtocolDecoderToyota* instance) {
    instance->decoder.parser_step = ToyotaStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->dm_state = ToyotaDmUnclocked;
    instance->preamble_shift = 0;
}

void tpms_protocol_decoder_toyota_reset(void* context) {
    furi_assert(context);
    toyota_reset_state(context);
}

// Returns -1 = no bit yet, 0 or 1 = decoded bit, -2 = sync lost / reset.
static int toyota_dm_advance(TPMSProtocolDecoderToyota* instance, uint32_t duration) {
    bool is_short = DURATION_DIFF(duration, tpms_protocol_toyota_const.te_short) <
                    tpms_protocol_toyota_const.te_delta;
    bool is_long = DURATION_DIFF(duration, tpms_protocol_toyota_const.te_long) <
                   tpms_protocol_toyota_const.te_delta;

    if(!is_short && !is_long) {
        instance->dm_state = ToyotaDmUnclocked;
        return -2;
    }

    if(is_long) {
        // Full bit period without a mid transition → "1".
        instance->dm_state = ToyotaDmAtBoundary;
        return 1;
    }

    // is_short
    if(instance->dm_state == ToyotaDmUnclocked) {
        // Can't tell start- vs mid-half until a te_long anchor arrives.
        return -1;
    }
    if(instance->dm_state == ToyotaDmAtBoundary) {
        // First half of a bit with mid-transition.
        instance->dm_state = ToyotaDmAtMid;
        return -1;
    }
    // At mid → second half done, full bit consumed with mid-transition → "0".
    instance->dm_state = ToyotaDmAtBoundary;
    return 0;
}

static void toyota_get_bytes(TPMSProtocolDecoderToyota* instance, uint8_t b[9]) {
    // 72 bits collected: head_64_bit lower 8 bits = stream bits 0..7 → b[0];
    // decode_data 64 bits = stream bits 8..71 → b[1..8].
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool toyota_check(TPMSProtocolDecoderToyota* instance) {
    uint8_t b[9];
    toyota_get_bytes(instance, b);
    // CRC-8, poly 0x07, init 0x80, over b[0..7].
    if(subghz_protocol_blocks_crc8(b, 8, 0x07, 0x80) != b[8]) return false;
    // Integrity: b[7] is the bitwise-inverted pressure byte.
    uint8_t pressure1 = (uint8_t)(((b[4] & 0x7F) << 1) | (b[5] >> 7));
    uint8_t pressure2 = (uint8_t)(b[7] ^ 0xFF);
    if(pressure1 != pressure2) return false;
    return true;
}

static void toyota_analyze(TPMSProtocolDecoderToyota* instance) {
    uint8_t b[9];
    toyota_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    uint8_t pressure1 = (uint8_t)(((b[4] & 0x7F) << 1) | (b[5] >> 7));
    uint8_t temp = (uint8_t)(((b[5] & 0x7F) << 1) | (b[6] >> 7));

    // pressure_PSI = pressure1 * 0.25 - 7.0 ; store as bar (PSI * 0.06895).
    float psi = (float)pressure1 * 0.25f - 7.0f;
    instance->generic.pressure = psi * 0.06895f;
    instance->generic.temperature = (float)((int)temp - 40);
    // Toyota exposes no defined battery-low bit; rtl_433 reports none.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_toyota_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    UNUSED(level);
    TPMSProtocolDecoderToyota* instance = context;

    int bit = toyota_dm_advance(instance, duration);
    if(bit == -2) {
        toyota_reset_state(instance);
        return;
    }
    if(bit == -1) return;

    switch(instance->decoder.parser_step) {
    case ToyotaStepReset:
        instance->preamble_shift = 0;
        instance->decoder.parser_step = ToyotaStepFindPreamble;
        /* fallthrough */
    case ToyotaStepFindPreamble:
        instance->preamble_shift =
            (uint8_t)(((instance->preamble_shift << 1) | (bit & 0x1)) & 0x3F);
        if(instance->preamble_shift == TOYOTA_PREAMBLE) {
            instance->decoder.parser_step = ToyotaStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case ToyotaStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, (uint8_t)bit, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_toyota_const.min_count_bit_for_found) {
            if(toyota_check(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                toyota_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            toyota_reset_state(instance);
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_toyota_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_toyota_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_toyota_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_toyota_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_toyota_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low == TPMS_NO_BATT ?
            "?" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
