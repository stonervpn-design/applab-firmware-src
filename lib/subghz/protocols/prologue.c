#include "prologue.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Prologue / ThermoPro-TX2 temperature-humidity weather sensor.
 *
 * Ported from the rtl_433 decoder src/devices/prologue.c (prologue_callback),
 * GPL-2.0-or-later. Also covers FreeTec NC-7104 / NC-7159-675, Pearl and TFA
 * pool thermometer 30.3240.10 sensors on 433.92 MHz OOK.
 *
 * RF profile (rtl_433 r_device prologue):
 *   - 433.92 MHz, OOK, PPM (distance / pulse-position coding)
 *   - Fixed ~500us HIGH pulse, then a gap that encodes the bit:
 *       ~2000us gap -> bit 0
 *       ~4000us gap -> bit 1
 *     sync gap ~9000us, reset ~10000us
 *   - 36 bits per row, the row is repeated ~7 times (a sync pulse precedes
 *     the first packet)
 *
 * Wire format, 9 nibbles = b[0..3] full + high nibble of b[4]:
 *   [type] [id0] [id1] [flags] [temp0] [temp1] [temp2] [humi0] [humi1]
 *   type   = b[0] >> 4          (fixed 0x9 or 0x5)
 *   id     = (b[0]&0x0F)<<4 | (b[1]>>4)
 *   flags  = b[1] low nibble: bit3 battery(1=OK) bit2 button bits1:0 channel-1
 *   temp 12-bit signed (x10) = (b[2]<<4) | (b[3]>>4), sign-extended
 *   humidity = (b[3]&0x0F)<<4 | (b[4]>>4)   (0xCC == no humidity sensor)
 *
 * Validation: Prologue has NO CRC. rtl_433's only structural gate is the
 * fixed type nibble (0x9 or 0x5). To keep the false-positive rate down on
 * noise we additionally require two consecutive identical 36-bit rows, which
 * mirrors rtl_433's bitbuffer_find_repeated_row(4, 36) requirement.
 *
 * Receive/decode-only: the encoder is an all-NULL stub.
 */


#define PROLOGUE_FRAME_BITS 36

#define PROLOGUE_PULSE_MIN 150
#define PROLOGUE_PULSE_MAX 1200
#define PROLOGUE_GAP_MIN   1000
#define PROLOGUE_GAP_MID   3000 /* < mid => 0, >= mid => 1 */
#define PROLOGUE_GAP_END   6000 /* >= end => row finished */
#define PROLOGUE_MAX_BITS  40

static const SubGhzBlockConst subghz_protocol_prologue_const = {
    .te_short = 2000,
    .te_long = 4000,
    .te_delta = 800,
    .min_count_bit_for_found = PROLOGUE_FRAME_BITS,
};

typedef enum {
    PrologueDecoderStepReset = 0,
    PrologueDecoderStepData,
} PrologueDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint64_t last_data;
    bool have_last;
} SubGhzProtocolDecoderPrologue;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_prologue_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_prologue_free(void* context);
static void subghz_protocol_decoder_prologue_reset(void* context);
static void subghz_protocol_decoder_prologue_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_prologue_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_prologue_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    subghz_protocol_decoder_prologue_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_decoder_prologue_get_string(void* context, FuriString* output);

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_prologue_decoder = {
    .alloc = subghz_protocol_decoder_prologue_alloc,
    .free = subghz_protocol_decoder_prologue_free,
    .feed = subghz_protocol_decoder_prologue_feed,
    .reset = subghz_protocol_decoder_prologue_reset,
    .get_hash_data = subghz_protocol_decoder_prologue_get_hash_data,
    .serialize = subghz_protocol_decoder_prologue_serialize,
    .deserialize = subghz_protocol_decoder_prologue_deserialize,
    .get_string = subghz_protocol_decoder_prologue_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_prologue_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_prologue = {
    .name = PROLOGUE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_prologue_decoder,
    .encoder = &subghz_protocol_prologue_encoder,
};

// ============================================================================
// Field extraction / validation (shared by decode + display)
// ============================================================================

typedef struct {
    uint8_t type;
    uint16_t id;
    uint8_t channel; /* 1..4 */
    bool battery_ok;
    bool button;
    float temp_c;
    int humidity; /* 0xCC == no humidity sensor */
} PrologueFields;

static void prologue_get_bytes(uint64_t data, uint8_t b[5]) {
    /* 36 bits, MSB-first: b[0..3] full, high nibble of b[4]. */
    b[0] = (uint8_t)((data >> 28) & 0xFF);
    b[1] = (uint8_t)((data >> 20) & 0xFF);
    b[2] = (uint8_t)((data >> 12) & 0xFF);
    b[3] = (uint8_t)((data >> 4) & 0xFF);
    b[4] = (uint8_t)((data & 0x0F) << 4);
}

static bool subghz_protocol_prologue_parse(uint64_t data, PrologueFields* out) {
    uint8_t b[5];
    prologue_get_bytes(data, b);

    /* Only structural gate rtl_433 has: type nibble must be 0x9 or 0x5. */
    if((b[0] & 0xF0) != 0x90 && (b[0] & 0xF0) != 0x50) return false;

    int temp_raw = (int16_t)((b[2] << 8) | (b[3] & 0xF0)); /* sign-extend */
    temp_raw = temp_raw >> 4;

    out->type = (uint8_t)(b[0] >> 4);
    out->id = (uint16_t)(((b[0] & 0x0F) << 4) | ((b[1] & 0xF0) >> 4));
    out->battery_ok = (b[1] & 0x08) != 0;
    out->button = ((b[1] & 0x04) >> 2) != 0;
    out->channel = (uint8_t)((b[1] & 0x03) + 1);
    out->temp_c = (float)temp_raw * 0.1f;
    out->humidity = (((b[3] & 0x0F) << 4) | (b[4] >> 4));
    return true;
}

// ============================================================================
// Complete-row processing
// ============================================================================

static void subghz_protocol_prologue_process(SubGhzProtocolDecoderPrologue* instance) {
    if(instance->decoder.decode_count_bit != PROLOGUE_FRAME_BITS) return;

    uint64_t data = instance->decoder.decode_data;

    PrologueFields fields;
    if(!subghz_protocol_prologue_parse(data, &fields)) {
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
    instance->generic.data_count_bit = PROLOGUE_FRAME_BITS;
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

static void* subghz_protocol_decoder_prologue_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPrologue* instance = calloc(1, sizeof(SubGhzProtocolDecoderPrologue));
    instance->base.protocol = &subghz_protocol_prologue;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void subghz_protocol_decoder_prologue_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_prologue_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;
    instance->decoder.parser_step = PrologueDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->have_last = false;
}

// ============================================================================
// feed - OOK PPM (distance coding) pulse-duration state machine
// ============================================================================

static void subghz_protocol_decoder_prologue_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;

    switch(instance->decoder.parser_step) {
    case PrologueDecoderStepReset:
        /* Start a row on a plausible ~500us HIGH pulse. */
        if(level && duration >= PROLOGUE_PULSE_MIN && duration <= PROLOGUE_PULSE_MAX) {
            instance->decoder.parser_step = PrologueDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case PrologueDecoderStepData:
        if(level) {
            /* HIGH pulse: only a ~500us mark is valid; anything else aborts. */
            if(duration < PROLOGUE_PULSE_MIN || duration > PROLOGUE_PULSE_MAX) {
                instance->decoder.parser_step = PrologueDecoderStepReset;
            }
        } else {
            /* LOW gap encodes the bit (or ends the row). */
            if(duration >= PROLOGUE_GAP_END) {
                subghz_protocol_prologue_process(instance);
                instance->decoder.parser_step = PrologueDecoderStepReset;
            } else if(duration >= PROLOGUE_GAP_MIN && duration < PROLOGUE_GAP_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
            } else if(duration >= PROLOGUE_GAP_MID) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
            } else {
                /* Too-short gap: noise, abandon row. */
                instance->decoder.parser_step = PrologueDecoderStepReset;
            }
            if(instance->decoder.decode_count_bit > PROLOGUE_MAX_BITS) {
                instance->decoder.parser_step = PrologueDecoderStepReset;
            }
        }
        break;
    }
}

// ============================================================================
// hash / serialize / deserialize / get_string
// ============================================================================

static uint8_t subghz_protocol_decoder_prologue_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_prologue_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    subghz_protocol_decoder_prologue_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_prologue_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_prologue_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderPrologue* instance = context;

    PrologueFields f = {0};
    bool ok = (instance->generic.data_count_bit == PROLOGUE_FRAME_BITS) &&
              subghz_protocol_prologue_parse(instance->generic.data, &f);
    if(!ok) {
        f.id = (uint16_t)instance->generic.serial;
        f.channel = (uint8_t)instance->generic.cnt;
        f.battery_ok = instance->generic.btn != 0;
        f.humidity = 0xCC;
        f.temp_c = 0;
    }

    if(f.humidity == 0xCC) {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%03X Ch:%u Bat:%s\r\n"
            "Temp:%.1f C",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            f.id,
            f.channel,
            f.battery_ok ? "OK" : "LOW",
            (double)f.temp_c);
    } else {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%03X Ch:%u Bat:%s\r\n"
            "Temp:%.1f C Hum:%d%%",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            f.id,
            f.channel,
            f.battery_ok ? "OK" : "LOW",
            (double)f.temp_c,
            f.humidity);
    }
}
