#include "acurite_tower.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Acurite 592TXR / 06002RM / 6044m "Tower" temperature + humidity sensor.
 *
 * Ported from rtl_433 src/devices/acurite.c (acurite_txr_decode / acurite_tower_decode,
 * Copyright rtl_433 authors, GPLv2+). Receive/decode-only.
 *
 * RF profile:
 *   - 433.92 MHz OOK, OOK_PULSE_PWM
 *   - short HIGH mark ~220us, long HIGH mark ~408us, sync HIGH mark ~620us
 *   - rtl_433 runs the PWM slicer (short->1, long->0) then bitbuffer_invert().
 *     Net result: short mark => data bit 0, long mark => data bit 1.
 *   - message: 7 bytes = 56 bits, MSB-first.
 *
 * Wire format (after invert), Tower/592TXR message type (bb[2]&0x3f == 0x04):
 *   byte0: CCII IIII   C=channel(2), I=id high(6)
 *   byte1: IIII IIII   id low(8)            -> id = ((b0&0x3f)<<8)|b1  (14 bit)
 *   byte2: pB00 0100   p=parity,B=batt ok,  msgtype low6 = 0x04
 *   byte3: pHHH HHHH   humidity = b3 & 0x7f
 *   byte4: p??T TTTT   temp high 7 bits
 *   byte5: pTTT TTTT   temp low  7 bits      -> raw = ((b4&0x7f)<<7)|(b5&0x7f)
 *                                               tempC = (raw - 1000) * 0.1
 *   byte6: KKKK KKKK   checksum = add_bytes(b,6) & 0xff
 *
 * Validation (matches rtl_433, keeps false positives away):
 *   1) msgtype (b2 & 0x3f) == 0x04
 *   2) checksum: (add_bytes(b,6) & 0xff) == b6
 *   3) even parity over bytes 2..5 (parity_bytes(&b[2],4) == 0)
 *   4) humidity <= 100 (127 = invalid-but-allowed marker)
 *   5) -40C <= tempC <= 70C
 *
 * This is a passive sensor: the encoder is an all-NULL stub (no TX).
 */

#define TAG "AcuriteTower"

#define ACURITE_TOWER_TE_SHORT 220
#define ACURITE_TOWER_TE_LONG  408
#define ACURITE_TOWER_TE_SYNC  620

/* Nearest-neighbour classification windows (us). */
#define ACURITE_TOWER_MARK_MIN       100
#define ACURITE_TOWER_SHORT_LONG_MID 314 /* (220+408)/2 */
#define ACURITE_TOWER_LONG_SYNC_MID  514 /* (408+620)/2 */
#define ACURITE_TOWER_SYNC_MAX       900
#define ACURITE_TOWER_GAP_RESET      700 /* > sync gap 596 & data gap 392, < packet gap 2192 */

#define ACURITE_TOWER_FRAME_BITS  56
#define ACURITE_TOWER_FRAME_BYTES 7
#define ACURITE_TOWER_MAX_BITS    72 /* runaway guard */

static const SubGhzBlockConst subghz_protocol_acurite_tower_const = {
    .te_short = ACURITE_TOWER_TE_SHORT,
    .te_long = ACURITE_TOWER_TE_LONG,
    .te_delta = 90,
    .min_count_bit_for_found = ACURITE_TOWER_FRAME_BITS,
};

typedef enum {
    AcuriteTowerDecoderStepReset = 0,
    AcuriteTowerDecoderStepData,
} AcuriteTowerDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolDecoderAcuriteTower;

// ============================================================================
// Frame extraction / validation (shared by decode + display)
// ============================================================================

typedef struct {
    uint16_t id;
    uint8_t channel; /* 0..3 -> C E B A */
    bool battery_ok;
    int humidity; /* percent RH */
    float temperature; /* Celsius */
} AcuriteTowerFrame;

static void acurite_tower_get_bytes(uint64_t data, uint8_t b[ACURITE_TOWER_FRAME_BYTES]) {
    for(int i = 0; i < ACURITE_TOWER_FRAME_BYTES; i++) {
        b[i] = (uint8_t)((data >> (8 * (ACURITE_TOWER_FRAME_BYTES - 1 - i))) & 0xFF);
    }
}

static bool subghz_protocol_acurite_tower_parse(
    uint64_t data,
    uint8_t count_bit,
    AcuriteTowerFrame* out) {
    if(count_bit != ACURITE_TOWER_FRAME_BITS) return false;

    uint8_t b[ACURITE_TOWER_FRAME_BYTES];
    acurite_tower_get_bytes(data, b);

    /* (1) message type must be Tower/592TXR temp+humidity */
    if((b[2] & 0x3f) != 0x04) return false;

    /* (2) 8-bit checksum in last byte */
    if((subghz_protocol_blocks_add_bytes(b, 6) & 0xff) != b[6]) return false;

    /* (3) even parity over bytes 2..5 */
    if(subghz_protocol_blocks_parity_bytes(&b[2], 4) != 0) return false;

    int humidity = b[3] & 0x7f;
    /* (4) humidity sanity: spec 1..99 percent, 127 is an allowed invalid marker */
    if(humidity > 100 && humidity != 127) return false;

    int temp_raw = ((b[4] & 0x7f) << 7) | (b[5] & 0x7f);
    float tempc = (float)(temp_raw - 1000) * 0.1f;
    /* (5) temperature range */
    if(tempc < -40.0f || tempc > 70.0f) return false;

    out->id = (uint16_t)(((b[0] & 0x3f) << 8) | b[1]);
    out->channel = (uint8_t)((b[0] & 0xC0) >> 6);
    out->battery_ok = (b[2] & 0x40) != 0;
    out->humidity = humidity;
    out->temperature = tempc;
    return true;
}

static const char* acurite_tower_channel_str(uint8_t ch) {
    static const char* const channel_strs[] = {"C", "E", "B", "A"}; /* E marks an error */
    return channel_strs[ch & 0x03];
}

// ============================================================================
// Decoder
// ============================================================================

static void* subghz_protocol_decoder_acurite_tower_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAcuriteTower* instance =
        malloc(sizeof(SubGhzProtocolDecoderAcuriteTower));
    instance->base.protocol = &subghz_protocol_acurite_tower;
    instance->generic.protocol_name = subghz_protocol_acurite_tower.name;
    instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    return instance;
}

static void subghz_protocol_decoder_acurite_tower_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_acurite_tower_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;
    instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

static void subghz_protocol_acurite_tower_process(SubGhzProtocolDecoderAcuriteTower* instance) {
    if(instance->decoder.decode_count_bit != ACURITE_TOWER_FRAME_BITS) return;

    AcuriteTowerFrame frame;
    if(!subghz_protocol_acurite_tower_parse(
           instance->decoder.decode_data, (uint8_t)instance->decoder.decode_count_bit, &frame)) {
        return;
    }

    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
    instance->generic.serial = frame.id;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static void subghz_protocol_decoder_acurite_tower_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;

    switch(instance->decoder.parser_step) {
    case AcuriteTowerDecoderStepReset:
        /* Wait for a sync HIGH mark to anchor the frame. */
        if(level && duration >= ACURITE_TOWER_LONG_SYNC_MID &&
           duration < ACURITE_TOWER_SYNC_MAX) {
            instance->decoder.parser_step = AcuriteTowerDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case AcuriteTowerDecoderStepData:
        if(level) {
            /* HIGH mark: classify by width. short->0, long->1 (post-invert). */
            if(duration < ACURITE_TOWER_MARK_MIN) {
                instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
            } else if(duration < ACURITE_TOWER_SHORT_LONG_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            } else if(duration < ACURITE_TOWER_LONG_SYNC_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else if(duration < ACURITE_TOWER_SYNC_MAX) {
                /* Another sync mark (preamble): restart bit collection. */
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else {
                /* Over-long mark: end of transmission. */
                subghz_protocol_acurite_tower_process(instance);
                instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
            }

            /* Only finalize while still collecting: the over-long/noise
             * branches above already reset and must not re-process. */
            if(instance->decoder.parser_step == AcuriteTowerDecoderStepData) {
                if(instance->decoder.decode_count_bit == ACURITE_TOWER_FRAME_BITS) {
                    subghz_protocol_acurite_tower_process(instance);
                    instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
                } else if(instance->decoder.decode_count_bit > ACURITE_TOWER_MAX_BITS) {
                    instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
                }
            }
        } else {
            /* Gap: a long low ends the row (abandon a partial frame). */
            if(duration >= ACURITE_TOWER_GAP_RESET) {
                instance->decoder.parser_step = AcuriteTowerDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            }
        }
        break;
    }
}

static uint8_t subghz_protocol_decoder_acurite_tower_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_acurite_tower_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus subghz_protocol_decoder_acurite_tower_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_acurite_tower_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_acurite_tower_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAcuriteTower* instance = context;

    AcuriteTowerFrame frame = {0};
    bool ok = subghz_protocol_acurite_tower_parse(
        instance->generic.data, (uint8_t)instance->generic.data_count_bit, &frame);

    if(ok) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%04X Ch:%s\r\n"
            "Temp:%.1f C Hum:%d%%\r\n"
            "Batt:%s",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            frame.id,
            acurite_tower_channel_str(frame.channel),
            (double)frame.temperature,
            frame.humidity,
            frame.battery_ok ? "ok" : "low");
    } else {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Raw:0x%014llX",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            (unsigned long long)instance->generic.data);
    }
}

// ============================================================================
// Registration (decode-only: encoder is an all-NULL stub)
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_acurite_tower_decoder = {
    .alloc = subghz_protocol_decoder_acurite_tower_alloc,
    .free = subghz_protocol_decoder_acurite_tower_free,
    .feed = subghz_protocol_decoder_acurite_tower_feed,
    .reset = subghz_protocol_decoder_acurite_tower_reset,
    .get_hash_data = subghz_protocol_decoder_acurite_tower_get_hash_data,
    .serialize = subghz_protocol_decoder_acurite_tower_serialize,
    .deserialize = subghz_protocol_decoder_acurite_tower_deserialize,
    .get_string = subghz_protocol_decoder_acurite_tower_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_acurite_tower_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_acurite_tower = {
    .name = ACURITE_TOWER_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_acurite_tower_decoder,
    .encoder = &subghz_protocol_acurite_tower_encoder,
};
