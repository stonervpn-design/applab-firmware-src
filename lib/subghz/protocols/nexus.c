#include "nexus.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Nexus temperature / humidity weather sensor.
 *
 * Ported from the rtl_433 decoder src/devices/nexus.c (nexus_decode),
 * GPL-2.0-or-later. Covers Nexus, FreeTec NC-7345 / NX-3980, Solight TE82S,
 * TFA 30.3209.02 and compatible 433.92 MHz OOK sensors.
 *
 * RF profile (rtl_433 r_device nexus):
 *   - 433.92 MHz, OOK, PPM (distance / pulse-position coding)
 *   - Fixed ~500us HIGH pulse, then a gap that encodes the bit:
 *       ~1000us gap -> bit 0
 *       ~2000us gap -> bit 1
 *     sync gap ~4000us, reset ~5000us
 *   - 36 bits per row, the row is repeated ~12 times
 *
 * Wire format, 9 nibbles = b[0..3] full + high nibble of b[4]:
 *   [id0 id1] [flags temp0] [temp1 temp2] [const humi0] [humi1 ...]
 *   b[0]         = 8-bit house id
 *   b[1] hi      = flags: bit7 battery(1=OK) bit6 test bits5:4 channel(0..2)
 *   temp 12-bit signed (scaled x10) = (b[1]&0x0F)<<8 | b[2]
 *   b[3] hi      = const 0xF (0b1111)
 *   humidity 8-bit = (b[3]&0x0F)<<4 | (b[4]>>4)
 *
 * Validation (exactly as rtl_433 - Nexus has no CRC of its own, so these
 * combined sanity gates are what suppress false positives):
 *   1) const nibble b[3] hi must be 0xF
 *   2) reject all-zero / all-0xFF id+temp+const combinations
 *   3) channel field must be 0..2 (i.e. (b[1]&0x30) != 0x30)
 *   4) ANTI-COLLISION CRC: the near-identical Rubicson / Solight-TE44 / EMOS
 *      family carries a real CRC-8 (poly 0x31, init 0x6c) in the last byte.
 *      If that CRC validates, this is one of theirs -> reject.
 *   5) humidity must be 0 or 1..100
 *   6) require two consecutive identical rows (mirrors rtl_433's
 *      bitbuffer_find_repeated_row) - a strong extra noise filter.
 *
 * Receive/decode-only: the encoder is an all-NULL stub.
 */


#define NEXUS_FRAME_BITS 36

#define NEXUS_PULSE_MIN 150
#define NEXUS_PULSE_MAX 1200
#define NEXUS_GAP_MIN   400
#define NEXUS_GAP_MID   1500 /* < mid => 0, >= mid => 1 */
#define NEXUS_GAP_END   3000 /* >= end => row finished */
#define NEXUS_MAX_BITS  40

static const SubGhzBlockConst subghz_protocol_nexus_const = {
    .te_short = 1000,
    .te_long = 2000,
    .te_delta = 400,
    .min_count_bit_for_found = NEXUS_FRAME_BITS,
};

typedef enum {
    NexusDecoderStepReset = 0,
    NexusDecoderStepData,
} NexusDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint64_t last_data;
    bool have_last;
} SubGhzProtocolDecoderNexus;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_nexus_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_nexus_free(void* context);
static void subghz_protocol_decoder_nexus_reset(void* context);
static void subghz_protocol_decoder_nexus_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_nexus_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_nexus_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    subghz_protocol_decoder_nexus_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_decoder_nexus_get_string(void* context, FuriString* output);

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_nexus_decoder = {
    .alloc = subghz_protocol_decoder_nexus_alloc,
    .free = subghz_protocol_decoder_nexus_free,
    .feed = subghz_protocol_decoder_nexus_feed,
    .reset = subghz_protocol_decoder_nexus_reset,
    .get_hash_data = subghz_protocol_decoder_nexus_get_hash_data,
    .serialize = subghz_protocol_decoder_nexus_serialize,
    .deserialize = subghz_protocol_decoder_nexus_deserialize,
    .get_string = subghz_protocol_decoder_nexus_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_nexus_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_nexus = {
    .name = NEXUS_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_nexus_decoder,
    .encoder = &subghz_protocol_nexus_encoder,
};

// ============================================================================
// Field extraction / validation (shared by decode + display)
// ============================================================================

typedef struct {
    uint8_t id;
    uint8_t channel; /* 1..3 */
    bool battery_ok;
    bool test;
    float temp_c;
    int humidity; /* 0 == no humidity sensor */
} NexusFields;

static void nexus_get_bytes(uint64_t data, uint8_t b[5]) {
    /* 36 bits, MSB-first: b[0..3] full, high nibble of b[4]. */
    b[0] = (uint8_t)((data >> 28) & 0xFF);
    b[1] = (uint8_t)((data >> 20) & 0xFF);
    b[2] = (uint8_t)((data >> 12) & 0xFF);
    b[3] = (uint8_t)((data >> 4) & 0xFF);
    b[4] = (uint8_t)((data & 0x0F) << 4);
}

static bool subghz_protocol_nexus_parse(uint64_t data, NexusFields* out) {
    uint8_t b[5];
    nexus_get_bytes(data, b);

    /* (1) const nibble */
    if((b[3] & 0xF0) != 0xF0) return false;

    /* (2) all-zero / all-0xFF reject */
    if((b[0] == 0 && b[2] == 0 && b[3] == 0) ||
       (b[0] == 0xFF && b[2] == 0xFF && b[3] == 0xFF))
        return false;

    /* (3) channel 1..3 */
    if((b[1] & 0x30) == 0x30) return false;

    /* (4) anti-collision: a valid Rubicson/Solight-TE44 CRC means it's theirs */
    uint8_t crc_in[5];
    crc_in[0] = b[0];
    crc_in[1] = b[1];
    crc_in[2] = b[2];
    crc_in[3] = b[3] & 0xF0;
    crc_in[4] = (uint8_t)(((b[3] & 0x0F) << 4) | ((b[4] & 0xF0) >> 4));
    if(subghz_protocol_blocks_crc8(crc_in, 5, 0x31, 0x6c) == 0) return false;

    int temp_raw = (int16_t)((b[1] << 12) | (b[2] << 4)); /* sign-extend */
    float temp_c = (float)(temp_raw >> 4) * 0.1f;
    int humidity = (((b[3] & 0x0F) << 4) | (b[4] >> 4));

    /* (5) humidity range */
    if(humidity != 0x00 && humidity > 100) return false;

    out->id = b[0];
    out->battery_ok = (b[1] & 0x80) != 0;
    out->test = (b[1] & 0x40) != 0;
    out->channel = (uint8_t)(((b[1] & 0x30) >> 4) + 1);
    out->temp_c = temp_c;
    out->humidity = humidity;
    return true;
}

// ============================================================================
// Complete-row processing
// ============================================================================

static void subghz_protocol_nexus_process(SubGhzProtocolDecoderNexus* instance) {
    if(instance->decoder.decode_count_bit != NEXUS_FRAME_BITS) return;

    uint64_t data = instance->decoder.decode_data;

    NexusFields fields;
    if(!subghz_protocol_nexus_parse(data, &fields)) {
        instance->have_last = false;
        return;
    }

    /* Require two consecutive identical valid rows (repeated-row confirmation). */
    if(!instance->have_last || instance->last_data != data) {
        instance->last_data = data;
        instance->have_last = true;
        return;
    }

    instance->generic.data = data;
    instance->generic.data_count_bit = NEXUS_FRAME_BITS;
    instance->generic.serial = fields.id;
    instance->generic.cnt = fields.channel;
    instance->generic.btn = fields.battery_ok ? 1 : 0;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// alloc / free / reset
// ============================================================================

static void* subghz_protocol_decoder_nexus_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderNexus* instance = calloc(1, sizeof(SubGhzProtocolDecoderNexus));
    instance->base.protocol = &subghz_protocol_nexus;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void subghz_protocol_decoder_nexus_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_nexus_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;
    instance->decoder.parser_step = NexusDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->have_last = false;
}

// ============================================================================
// feed - OOK PPM (distance coding) pulse-duration state machine
// ============================================================================

static void subghz_protocol_decoder_nexus_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;

    switch(instance->decoder.parser_step) {
    case NexusDecoderStepReset:
        /* Start a row on a plausible ~500us HIGH pulse. */
        if(level && duration >= NEXUS_PULSE_MIN && duration <= NEXUS_PULSE_MAX) {
            instance->decoder.parser_step = NexusDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case NexusDecoderStepData:
        if(level) {
            /* HIGH pulse: only a ~500us mark is valid; anything else aborts. */
            if(duration < NEXUS_PULSE_MIN || duration > NEXUS_PULSE_MAX) {
                instance->decoder.parser_step = NexusDecoderStepReset;
            }
        } else {
            /* LOW gap encodes the bit (or ends the row). */
            if(duration >= NEXUS_GAP_END) {
                subghz_protocol_nexus_process(instance);
                instance->decoder.parser_step = NexusDecoderStepReset;
            } else if(duration >= NEXUS_GAP_MIN && duration < NEXUS_GAP_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            } else if(duration >= NEXUS_GAP_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else {
                /* Too-short gap: noise, abandon row. */
                instance->decoder.parser_step = NexusDecoderStepReset;
            }
            if(instance->decoder.decode_count_bit > NEXUS_MAX_BITS) {
                instance->decoder.parser_step = NexusDecoderStepReset;
            }
        }
        break;
    }
}

// ============================================================================
// hash / serialize / deserialize / get_string
// ============================================================================

static uint8_t subghz_protocol_decoder_nexus_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_nexus_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    subghz_protocol_decoder_nexus_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_nexus_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_nexus_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderNexus* instance = context;

    NexusFields f = {0};
    bool ok = (instance->generic.data_count_bit == NEXUS_FRAME_BITS) &&
              subghz_protocol_nexus_parse(instance->generic.data, &f);
    if(!ok) {
        f.id = (uint8_t)instance->generic.serial;
        f.channel = (uint8_t)instance->generic.cnt;
        f.battery_ok = instance->generic.btn != 0;
        f.humidity = 0;
        f.temp_c = 0;
    }

    furi_string_printf(
        output,
        "%s %dbit\r\n"
        "Id:0x%02X Ch:%u Bat:%s\r\n"
        "Temp:%.1f C Hum:%d%%",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        f.id,
        f.channel,
        f.battery_ok ? "OK" : "LOW",
        (double)f.temp_c,
        f.humidity);
}
