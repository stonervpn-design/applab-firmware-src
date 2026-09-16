#include "oregon_thgr.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <lib/toolbox/manchester_decoder.h>

/*
 * Oregon Scientific v2.1 weather sensors (THGR / THN family).
 *
 * Ported from the in-tree Flipper decoder
 *   applications_user/weather_station/protocols/oregon2.c
 * (itself following rtl_433/src/devices/oregon_scientific.c, GPL-2+), re-homed
 * into the lib/subghz protocol registry as a decode-only Static sensor so it
 * can be surfaced in the RAM-gated Sensors set.
 *
 * RF profile: 433.92 MHz OOK, Manchester at ~1024 baud. Oregon v2.1 doubles
 * every data bit (differential Manchester); we recover the clock with the
 * shared ManchesterState helper, then de-double consecutive bits (01 -> 1,
 * 10 -> 0). A 19-bit preamble (15 ones + 0101) anchors the frame; the 32-bit
 * fixed part carries sensor type / channel / rolling-id / battery; a
 * sensor-specific variable part carries BCD temperature and humidity followed
 * by an 8-bit "sum of nibbles" checksum, which we verify before display.
 *
 * Covered sensors (temperature / humidity):
 *   THGR122N / THGR228N (0x1d20) : temp + humidity
 *   THR228N  / THN132N  (0xec40) : temp only
 *
 * Receive/decode-only; the encoder is a NULL stub.
 */

#define TAG "OregonTHGR"

static const SubGhzBlockConst subghz_protocol_oregon_thgr_const = {
    .te_long = 1000,
    .te_short = 500,
    .te_delta = 200,
    .min_count_bit_for_found = 32,
};

#define OREGON2_PREAMBLE_BITS 19
#define OREGON2_PREAMBLE_MASK 0b1111111111111111111
// 15 ones + 0101 (inverted A)
#define OREGON2_PREAMBLE      0b1111111111111110101
#define OREGON2_SENSOR_ID(d)  (((d) >> 16) & 0xFFFF)
#define OREGON2_CHECKSUM_BITS 8
#define OREGON2_FLAG_BAT_LOW  0x4

// Sensor type ids
#define ID_THGR122N 0x1d20
#define ID_THR228N  0xec40

struct SubGhzProtocolDecoderOregonTHGR {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    bool prev_bit;
    bool have_bit;
    uint8_t var_bits;
    uint32_t var_data;
};

typedef struct SubGhzProtocolDecoderOregonTHGR SubGhzProtocolDecoderOregonTHGR;

typedef enum {
    OregonTHGRStepReset = 0,
    OregonTHGRStepFoundPreamble,
    OregonTHGRStepVarData,
} OregonTHGRStep;

// ============================================================================
// Field helpers (identical math to rtl_433 / oregon2.c)
// ============================================================================

// From sensor id, amount of bits in the variable (sensor-specific) section.
static uint8_t oregon_thgr_var_bits(uint16_t sensor_id) {
    switch(sensor_id) {
    case ID_THR228N:
        return 16; // temp only
    case ID_THGR122N:
        return 24; // temp + humidity
    default:
        return 0;
    }
}

static uint16_t oregon_thgr_bcd_short(uint32_t data) {
    return (data & 0xF) * 10 + ((data >> 4) & 0xF);
}

static float oregon_thgr_decode_temp(uint32_t data) {
    int32_t temp_val = oregon_thgr_bcd_short(data >> 4);
    temp_val *= 10;
    temp_val += (data >> 12) & 0xF;
    if(data & 0xF) temp_val = -temp_val;
    return (float)temp_val / 10.0f;
}

static uint8_t oregon_thgr_channel(uint32_t fixed) {
    uint8_t ch_bits = (fixed >> 12) & 0xF;
    uint8_t channel = 1;
    while(ch_bits > 1) {
        channel++;
        ch_bits >>= 1;
    }
    return channel;
}

// Verify the swapped-nibble "sum of nibbles" checksum. Returns true if valid.
static bool oregon_thgr_check_sum(uint32_t fix_data, uint32_t var_data) {
    uint8_t sum = fix_data & 0xF;
    uint8_t ref_sum = var_data & 0xFF;
    var_data >>= 8;

    for(uint8_t i = 1; i < 8; i++) {
        fix_data >>= 4;
        var_data >>= 4;
        sum += (fix_data & 0xF) + (var_data & 0xF);
    }
    // swap calculated sum nibbles
    sum = (((sum >> 4) & 0xF) | (sum << 4)) & 0xFF;
    return sum == ref_sum;
}

// ============================================================================
// Decoder callbacks
// ============================================================================

static void* subghz_protocol_decoder_oregon_thgr_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderOregonTHGR* instance = malloc(sizeof(SubGhzProtocolDecoderOregonTHGR));
    instance->base.protocol = &subghz_protocol_oregon_thgr;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_state = ManchesterStateStart1;
    instance->prev_bit = false;
    instance->have_bit = false;
    instance->var_bits = 0;
    instance->var_data = 0;
    return instance;
}

static void subghz_protocol_decoder_oregon_thgr_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_oregon_thgr_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;
    instance->decoder.parser_step = OregonTHGRStepReset;
    instance->decoder.decode_data = 0UL;
    instance->decoder.decode_count_bit = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
    instance->have_bit = false;
    instance->var_data = 0;
    instance->var_bits = 0;
}

static ManchesterEvent oregon_thgr_event(bool level, uint32_t duration) {
    bool is_long;
    if(DURATION_DIFF(duration, subghz_protocol_oregon_thgr_const.te_long) <
       subghz_protocol_oregon_thgr_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, subghz_protocol_oregon_thgr_const.te_short) <
        subghz_protocol_oregon_thgr_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Reverse the nibbles of a 32-bit word (Oregon is LSB-first within nibbles).
static uint32_t oregon_thgr_reflect_nibbles(uint32_t d) {
    d = (d & 0x55555555) << 1 | (d & 0xAAAAAAAA) >> 1;
    d = (d & 0x33333333) << 2 | (d & 0xCCCCCCCC) >> 2;
    return d;
}

static void subghz_protocol_decoder_oregon_thgr_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;

    // Oregon v2.1 signal is inverted.
    ManchesterEvent event = oregon_thgr_event(!level, duration);
    bool data;

    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = OregonTHGRStepReset;
        instance->have_bit = false;
        instance->decoder.decode_data = 0UL;
        instance->decoder.decode_count_bit = 0;
    }

    if(manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data)) {
        // v2.1 doubles each bit: de-double consecutive Manchester bits.
        if(instance->have_bit) {
            if(!instance->prev_bit && data) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else if(instance->prev_bit && !data) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            } else {
                subghz_protocol_decoder_oregon_thgr_reset(context);
            }
            instance->have_bit = false;
        } else {
            instance->prev_bit = data;
            instance->have_bit = true;
        }
    }

    switch(instance->decoder.parser_step) {
    case OregonTHGRStepReset:
        if(instance->decoder.decode_count_bit >= OREGON2_PREAMBLE_BITS &&
           ((instance->decoder.decode_data & OREGON2_PREAMBLE_MASK) == OREGON2_PREAMBLE)) {
            instance->decoder.parser_step = OregonTHGRStepFoundPreamble;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.decode_data = 0UL;
        }
        break;

    case OregonTHGRStepFoundPreamble:
        if(instance->decoder.decode_count_bit == 32) {
            instance->generic.data =
                oregon_thgr_reflect_nibbles((uint32_t)instance->decoder.decode_data);
            instance->generic.data_count_bit = 32;
            instance->decoder.decode_data = 0UL;
            instance->decoder.decode_count_bit = 0;

            instance->generic.serial = OREGON2_SENSOR_ID(instance->generic.data);
            instance->var_bits = oregon_thgr_var_bits((uint16_t)instance->generic.serial);

            if(!instance->var_bits) {
                // Unsupported/other sensor: stop (do not emit a bare fixed part).
                instance->decoder.parser_step = OregonTHGRStepReset;
            } else {
                instance->decoder.parser_step = OregonTHGRStepVarData;
            }
        }
        break;

    case OregonTHGRStepVarData:
        if(instance->decoder.decode_count_bit == instance->var_bits + OREGON2_CHECKSUM_BITS) {
            instance->var_data =
                oregon_thgr_reflect_nibbles((uint32_t)(instance->decoder.decode_data & 0xFFFFFFFF));

            instance->decoder.parser_step = OregonTHGRStepReset;

            // Enforce the checksum before the callback so noise can't emit.
            if(oregon_thgr_check_sum((uint32_t)instance->generic.data, instance->var_data)) {
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
        }
        break;
    }
}

static uint8_t subghz_protocol_decoder_oregon_thgr_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_oregon_thgr_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;
    uint32_t temp = instance->var_bits;
    if(!flipper_format_write_uint32(flipper_format, "VarBits", &temp, 1)) {
        FURI_LOG_E(TAG, "Error adding VarBits");
        return SubGhzProtocolStatusErrorParserOthers;
    }
    if(!flipper_format_write_hex(
           flipper_format, "VarData", (const uint8_t*)&instance->var_data, sizeof(instance->var_data))) {
        FURI_LOG_E(TAG, "Error adding VarData");
        return SubGhzProtocolStatusErrorParserOthers;
    }
    return ret;
}

static SubGhzProtocolStatus subghz_protocol_decoder_oregon_thgr_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;
    uint32_t temp_data;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_oregon_thgr_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;
        if(!flipper_format_read_uint32(flipper_format, "VarBits", &temp_data, 1)) {
            FURI_LOG_E(TAG, "Missing VarBits");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->var_bits = (uint8_t)temp_data;
        if(!flipper_format_read_hex(
               flipper_format, "VarData", (uint8_t*)&instance->var_data, sizeof(instance->var_data))) {
            FURI_LOG_E(TAG, "Missing VarData");
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->generic.serial = OREGON2_SENSOR_ID(instance->generic.data);
    } while(false);
    return ret;
}

static void subghz_protocol_decoder_oregon_thgr_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderOregonTHGR* instance = context;

    uint32_t fixed = (uint32_t)instance->generic.data;
    uint16_t sensor_id = OREGON2_SENSOR_ID(fixed);
    uint8_t channel = oregon_thgr_channel(fixed);
    uint8_t battery_low = (fixed & OREGON2_FLAG_BAT_LOW) ? 1 : 0;

    furi_string_cat_printf(
        output,
        "%s\r\n"
        "ID:0x%04X ch:%d bat:%d rc:0x%02lX\r\n",
        instance->generic.protocol_name,
        sensor_id,
        channel,
        battery_low,
        (uint32_t)(fixed >> 4) & 0xFF);

    if(instance->var_bits > 0) {
        uint32_t payload = instance->var_data >> OREGON2_CHECKSUM_BITS;
        float temp;
        int humidity = -1;
        if(sensor_id == ID_THGR122N) {
            humidity = oregon_thgr_bcd_short(payload);
            temp = oregon_thgr_decode_temp(payload >> 8);
        } else {
            temp = oregon_thgr_decode_temp(payload);
        }

        int t10 = (int)((temp < 0 ? -temp : temp) * 10.0f + 0.5f);
        const char* sign = (temp < 0) ? "-" : "";
        bool sum_ok = oregon_thgr_check_sum(fixed, instance->var_data);

        if(humidity >= 0) {
            furi_string_cat_printf(
                output,
                "Temp:%s%d.%d C Hum:%d%%\r\n"
                "Sum:%s",
                sign,
                t10 / 10,
                t10 % 10,
                humidity,
                sum_ok ? "ok" : "err");
        } else {
            furi_string_cat_printf(
                output,
                "Temp:%s%d.%d C\r\n"
                "Sum:%s",
                sign,
                t10 / 10,
                t10 % 10,
                sum_ok ? "ok" : "err");
        }
    }
}

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_oregon_thgr_decoder = {
    .alloc = subghz_protocol_decoder_oregon_thgr_alloc,
    .free = subghz_protocol_decoder_oregon_thgr_free,
    .feed = subghz_protocol_decoder_oregon_thgr_feed,
    .reset = subghz_protocol_decoder_oregon_thgr_reset,
    .get_hash_data = subghz_protocol_decoder_oregon_thgr_get_hash_data,
    .serialize = subghz_protocol_decoder_oregon_thgr_serialize,
    .deserialize = subghz_protocol_decoder_oregon_thgr_deserialize,
    .get_string = subghz_protocol_decoder_oregon_thgr_get_string,
};

// Decode-only: NULL encoder (passive receive; no TX).
const SubGhzProtocolEncoder subghz_protocol_oregon_thgr_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_oregon_thgr = {
    .name = OREGON_THGR_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_oregon_thgr_decoder,
    .encoder = &subghz_protocol_oregon_thgr_encoder,
};
