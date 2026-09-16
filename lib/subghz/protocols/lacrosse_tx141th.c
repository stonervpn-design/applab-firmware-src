#include "lacrosse_tx141th.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * LaCrosse TX141TH-Bv2 temperature + humidity sensor (and TFA 30.3221/30.3249 clones).
 *
 * Ported from rtl_433 src/devices/lacrosse_tx141x.c (lacrosse_tx141x_decode,
 * Copyright rtl_433 authors, GPLv2+). Receive/decode-only.
 *
 * RF profile:
 *   - 433.92 MHz OOK, OOK_PULSE_PWM, fixed period ~625us
 *   - short HIGH mark ~208us, long HIGH mark ~417us, preamble sync mark ~833us (x4)
 *   - rtl_433 runs the PWM slicer (short->1, long->0) then bitbuffer_invert().
 *     Net result: short mark => data bit 0, long mark => data bit 1.
 *     (A logical 1 is 417us high; a logical 0 is 208us high.)
 *   - message: 5 bytes = 40 bits, MSB-first. 12 identical packets per burst.
 *
 * Wire format (after invert), TX141TH-Bv2:
 *   byte0: IIII IIII   id (8 bit)
 *   byte1: BTCC TTTT   B=battery low, T(bit6)=test button, C=channel(2),
 *                      low nibble = temp high 4 bits
 *   byte2: TTTT TTTT   temp low 8 bits   -> raw = ((b1&0x0f)<<8)|b2
 *                                            tempC = (raw - 500) * 0.1
 *   byte3: HHHH HHHH   humidity (percent RH)
 *   byte4: KKKK KKKK   digest = lfsr_digest8_reflect(b, 4, 0x31, 0xf4)
 *
 * Validation (matches rtl_433):
 *   1) digest: lfsr_digest8_reflect(b,4,0x31,0xf4) == b4
 *   2) id != 0
 *   3) 1 <= humidity <= 100
 *   4) -40C <= tempC <= 140C
 *
 * This is a passive sensor: the encoder is an all-NULL stub (no TX).
 */

#define TAG "LaCrosseTX141TH"

#define LACROSSE_TX141TH_TE_SHORT 208
#define LACROSSE_TX141TH_TE_LONG  417
#define LACROSSE_TX141TH_TE_SYNC  833

/* Nearest-neighbour classification windows (us). */
#define LACROSSE_TX141TH_MARK_MIN       100
#define LACROSSE_TX141TH_SHORT_LONG_MID 312 /* (208+417)/2 */
#define LACROSSE_TX141TH_LONG_SYNC_MID  625 /* (417+833)/2 */
#define LACROSSE_TX141TH_SYNC_MAX       1200
#define LACROSSE_TX141TH_GAP_RESET      900 /* > data gap 417, < packet reset 1700 */

#define LACROSSE_TX141TH_FRAME_BITS  40
#define LACROSSE_TX141TH_FRAME_BYTES 5
#define LACROSSE_TX141TH_MAX_BITS    56 /* runaway guard */

static const SubGhzBlockConst subghz_protocol_lacrosse_tx141th_const = {
    .te_short = LACROSSE_TX141TH_TE_SHORT,
    .te_long = LACROSSE_TX141TH_TE_LONG,
    .te_delta = 100,
    .min_count_bit_for_found = LACROSSE_TX141TH_FRAME_BITS,
};

typedef enum {
    LaCrosseTx141thDecoderStepReset = 0,
    LaCrosseTx141thDecoderStepData,
} LaCrosseTx141thDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolDecoderLaCrosseTx141th;

// ============================================================================
// Frame extraction / validation (shared by decode + display)
// ============================================================================

typedef struct {
    uint8_t id;
    uint8_t channel; /* 0..3 */
    bool battery_ok;
    bool test;
    int humidity; /* percent RH */
    float temperature; /* Celsius */
} LaCrosseTx141thFrame;

static void
    lacrosse_tx141th_get_bytes(uint64_t data, uint8_t b[LACROSSE_TX141TH_FRAME_BYTES]) {
    for(int i = 0; i < LACROSSE_TX141TH_FRAME_BYTES; i++) {
        b[i] = (uint8_t)((data >> (8 * (LACROSSE_TX141TH_FRAME_BYTES - 1 - i))) & 0xFF);
    }
}

static bool subghz_protocol_lacrosse_tx141th_parse(
    uint64_t data,
    uint8_t count_bit,
    LaCrosseTx141thFrame* out) {
    if(count_bit != LACROSSE_TX141TH_FRAME_BITS) return false;

    uint8_t b[LACROSSE_TX141TH_FRAME_BYTES];
    lacrosse_tx141th_get_bytes(data, b);

    /* (1) LFSR digest check */
    if(subghz_protocol_blocks_lfsr_digest8_reflect(b, 4, 0x31, 0xf4) != b[4]) return false;

    int id = b[0];
    int battery_low = (b[1] >> 7);
    int test = (b[1] & 0x40) >> 6;
    int channel = (b[1] & 0x30) >> 4;
    int temp_raw = ((b[1] & 0x0f) << 8) | b[2];
    float temp_c = (float)(temp_raw - 500) * 0.1f;
    int humidity = b[3];

    /* (2..4) sanity to keep noise out */
    if(id == 0) return false;
    if(humidity == 0 || humidity > 100) return false;
    if(temp_c < -40.0f || temp_c > 140.0f) return false;

    out->id = (uint8_t)id;
    out->channel = (uint8_t)channel;
    out->battery_ok = (battery_low == 0);
    out->test = (test != 0);
    out->humidity = humidity;
    out->temperature = temp_c;
    return true;
}

// ============================================================================
// Decoder
// ============================================================================

static void* subghz_protocol_decoder_lacrosse_tx141th_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLaCrosseTx141th* instance =
        malloc(sizeof(SubGhzProtocolDecoderLaCrosseTx141th));
    instance->base.protocol = &subghz_protocol_lacrosse_tx141th;
    instance->generic.protocol_name = subghz_protocol_lacrosse_tx141th.name;
    instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    return instance;
}

static void subghz_protocol_decoder_lacrosse_tx141th_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_lacrosse_tx141th_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;
    instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

static void
    subghz_protocol_lacrosse_tx141th_process(SubGhzProtocolDecoderLaCrosseTx141th* instance) {
    if(instance->decoder.decode_count_bit != LACROSSE_TX141TH_FRAME_BITS) return;

    LaCrosseTx141thFrame frame;
    if(!subghz_protocol_lacrosse_tx141th_parse(
           instance->decoder.decode_data, (uint8_t)instance->decoder.decode_count_bit, &frame)) {
        return;
    }

    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
    instance->generic.serial = frame.id;
    instance->generic.btn = frame.test ? 1 : 0;
    instance->generic.cnt = 0;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static void subghz_protocol_decoder_lacrosse_tx141th_feed(
    void* context,
    bool level,
    uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;

    switch(instance->decoder.parser_step) {
    case LaCrosseTx141thDecoderStepReset:
        /* Wait for a preamble sync HIGH mark to anchor the frame. */
        if(level && duration >= LACROSSE_TX141TH_LONG_SYNC_MID &&
           duration < LACROSSE_TX141TH_SYNC_MAX) {
            instance->decoder.parser_step = LaCrosseTx141thDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case LaCrosseTx141thDecoderStepData:
        if(level) {
            /* HIGH mark: classify by width. short->0, long->1 (post-invert). */
            if(duration < LACROSSE_TX141TH_MARK_MIN) {
                instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
            } else if(duration < LACROSSE_TX141TH_SHORT_LONG_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            } else if(duration < LACROSSE_TX141TH_LONG_SYNC_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else if(duration < LACROSSE_TX141TH_SYNC_MAX) {
                /* Another preamble sync mark: restart bit collection. */
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else {
                /* Over-long mark: end of transmission. */
                subghz_protocol_lacrosse_tx141th_process(instance);
                instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
            }

            /* Only finalize while still collecting: the over-long/noise
             * branches above already reset and must not re-process. */
            if(instance->decoder.parser_step == LaCrosseTx141thDecoderStepData) {
                if(instance->decoder.decode_count_bit == LACROSSE_TX141TH_FRAME_BITS) {
                    subghz_protocol_lacrosse_tx141th_process(instance);
                    instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
                } else if(instance->decoder.decode_count_bit > LACROSSE_TX141TH_MAX_BITS) {
                    instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
                }
            }
        } else {
            /* Gap: a long low ends the row (abandon a partial frame). */
            if(duration >= LACROSSE_TX141TH_GAP_RESET) {
                instance->decoder.parser_step = LaCrosseTx141thDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            }
        }
        break;
    }
}

static uint8_t subghz_protocol_decoder_lacrosse_tx141th_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_lacrosse_tx141th_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus subghz_protocol_decoder_lacrosse_tx141th_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_lacrosse_tx141th_const.min_count_bit_for_found);
}

static void
    subghz_protocol_decoder_lacrosse_tx141th_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderLaCrosseTx141th* instance = context;

    LaCrosseTx141thFrame frame = {0};
    bool ok = subghz_protocol_lacrosse_tx141th_parse(
        instance->generic.data, (uint8_t)instance->generic.data_count_bit, &frame);

    if(ok) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%02X Ch:%d\r\n"
            "Temp:%.1f C Hum:%d%%\r\n"
            "Batt:%s Test:%s",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            frame.id,
            frame.channel,
            (double)frame.temperature,
            frame.humidity,
            frame.battery_ok ? "ok" : "low",
            frame.test ? "yes" : "no");
    } else {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Raw:0x%010llX",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            (unsigned long long)instance->generic.data);
    }
}

// ============================================================================
// Registration (decode-only: encoder is an all-NULL stub)
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_lacrosse_tx141th_decoder = {
    .alloc = subghz_protocol_decoder_lacrosse_tx141th_alloc,
    .free = subghz_protocol_decoder_lacrosse_tx141th_free,
    .feed = subghz_protocol_decoder_lacrosse_tx141th_feed,
    .reset = subghz_protocol_decoder_lacrosse_tx141th_reset,
    .get_hash_data = subghz_protocol_decoder_lacrosse_tx141th_get_hash_data,
    .serialize = subghz_protocol_decoder_lacrosse_tx141th_serialize,
    .deserialize = subghz_protocol_decoder_lacrosse_tx141th_deserialize,
    .get_string = subghz_protocol_decoder_lacrosse_tx141th_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_lacrosse_tx141th_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_lacrosse_tx141th = {
    .name = LACROSSE_TX141TH_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_lacrosse_tx141th_decoder,
    .encoder = &subghz_protocol_lacrosse_tx141th_encoder,
};
