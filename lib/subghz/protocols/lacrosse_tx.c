#include "lacrosse_tx.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * LaCrosse TX 433 MHz Temperature / Humidity sensors (TX-3/4/6U/7U, also
 * TFA Dostmann 30.3120.90 / 30.3121).
 *
 * Ported from rtl_433 src/devices/lacrosse.c (Robert C. Terzi, 2015, GPLv2+).
 *
 * RF profile:
 *   - 433.92 MHz OOK, PWM (rtl_433 OOK_PULSE_PWM)
 *   - bit "1" : short HIGH ~550 us  + ~1000 us gap
 *   - bit "0" : long  HIGH ~1400 us + ~1000 us gap
 *   - inter-bit gap ~1000 us; packet gap ~29 ms (reset)
 *   - Message: 44 bits = 11 x 4-bit nibbles, one repeat after ~30 ms
 *
 * Nibble layout (MSB-first):
 *   [0=0x0][1=0xA][type][addr][addr+parity][v1][v2][v3][iv1][iv2][check]
 *     nibble0..1 : fixed 0x0A start byte
 *     nibble2    : message type (0x00 = temperature, 0x0E = humidity)
 *     nibble3..4 : 11-bit sensor id + parity bit (id = (n3<<3)|(n4>>1))
 *     nibble5..7 : BCD value v = n5*10 + n6 + n7*0.1
 *     nibble8..9 : integer repeat of the value (iv = n8*10 + n9)
 *     nibble10   : 4-bit checksum = (sum of nibbles 0..9) & 0x0F
 *   parity: (popcount(n5,n6,n7) + (n4 & 1)) must be even
 *
 * Temperature (type 0x00): Celsius = v - 50.0
 * Humidity    (type 0x0E): percent = v
 *
 * Integrity (all enforced before the callback, per rtl_433): start byte 0x0A,
 * 4-bit checksum, parity, and the v/iv redundancy (n5==n8 && n6==n9).
 *
 * Receive/decode-only (passive sensor); the encoder is an all-NULL stub.
 */

#define TAG "LaCrosseTX"

#define LACROSSE_BIT_SHORT 550 /* short mark => bit 1 */
#define LACROSSE_BIT_LONG  1400 /* long mark  => bit 0 */

#define LACROSSE_MARK_MIN 250
#define LACROSSE_MARK_MID 975 /* (550+1400)/2 */
#define LACROSSE_MARK_MAX 2000
#define LACROSSE_GAP_RESET 3000 /* gap >= this ends the packet row */

#define LACROSSE_DATA_BITS 44
#define LACROSSE_MAX_BITS  52 /* runaway guard */

static const SubGhzBlockConst subghz_protocol_lacrosse_tx_const = {
    .te_short = LACROSSE_BIT_SHORT,
    .te_long = LACROSSE_BIT_LONG,
    .te_delta = 250,
    .min_count_bit_for_found = LACROSSE_DATA_BITS,
};

typedef enum {
    LacrosseDecoderStepReset = 0,
    LacrosseDecoderStepData,
} LacrosseDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolDecoderLacrosseTx;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_lacrosse_tx_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_lacrosse_tx_free(void* context);
static void subghz_protocol_decoder_lacrosse_tx_reset(void* context);
static void
    subghz_protocol_decoder_lacrosse_tx_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_lacrosse_tx_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_lacrosse_tx_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    subghz_protocol_decoder_lacrosse_tx_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_decoder_lacrosse_tx_get_string(void* context, FuriString* output);

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_lacrosse_tx_decoder = {
    .alloc = subghz_protocol_decoder_lacrosse_tx_alloc,
    .free = subghz_protocol_decoder_lacrosse_tx_free,
    .feed = subghz_protocol_decoder_lacrosse_tx_feed,
    .reset = subghz_protocol_decoder_lacrosse_tx_reset,
    .get_hash_data = subghz_protocol_decoder_lacrosse_tx_get_hash_data,
    .serialize = subghz_protocol_decoder_lacrosse_tx_serialize,
    .deserialize = subghz_protocol_decoder_lacrosse_tx_deserialize,
    .get_string = subghz_protocol_decoder_lacrosse_tx_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_lacrosse_tx_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_lacrosse_tx = {
    .name = LACROSSE_TX_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_lacrosse_tx_decoder,
    .encoder = &subghz_protocol_lacrosse_tx_encoder,
};

// ============================================================================
// Decode helpers
// ============================================================================

static void subghz_protocol_lacrosse_tx_nibbles(uint64_t data, uint8_t n[11]) {
    for(int i = 0; i < 11; i++) {
        n[i] = (uint8_t)((data >> (4 * (10 - i))) & 0x0F);
    }
}

static uint8_t subghz_protocol_lacrosse_tx_popcount4(uint8_t nib) {
    nib &= 0x0F;
    uint8_t c = 0;
    while(nib) {
        c += nib & 1U;
        nib >>= 1;
    }
    return c;
}

/* Validate one 44-bit frame exactly as rtl_433 does. */
static bool subghz_protocol_lacrosse_tx_check(uint64_t data) {
    uint8_t n[11];
    subghz_protocol_lacrosse_tx_nibbles(data, n);

    /* Start byte must be 0x0A -> nibble0 = 0x0, nibble1 = 0xA. */
    if(n[0] != 0x0 || n[1] != 0xA) return false;

    /* 4-bit checksum over nibbles 0..9. */
    uint8_t checksum = 0;
    for(int i = 0; i < 10; i++) checksum = (uint8_t)((checksum + n[i]) & 0x0F);
    if(checksum != n[10]) return false;

    /* Parity over the three value nibbles + the parity bit in nibble4. */
    uint8_t parity = subghz_protocol_lacrosse_tx_popcount4(n[5]) +
                     subghz_protocol_lacrosse_tx_popcount4(n[6]) +
                     subghz_protocol_lacrosse_tx_popcount4(n[7]) + (n[4] & 0x01);
    if(parity % 2 != 0) return false;

    /* Redundancy: integer repeat must match the value. */
    if(n[5] != n[8] || n[6] != n[9]) return false;

    /* Only temperature (0x00) and humidity (0x0E) message types are known. */
    uint8_t type = n[2];
    if(type != 0x00 && type != 0x0E) return false;

    return true;
}

static void subghz_protocol_decoder_lacrosse_tx_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;
    instance->decoder.parser_step = LacrosseDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

static void
    subghz_protocol_lacrosse_tx_finalize(SubGhzProtocolDecoderLacrosseTx* instance) {
    if(instance->decoder.decode_count_bit != LACROSSE_DATA_BITS) return;
    if(!subghz_protocol_lacrosse_tx_check(instance->decoder.decode_data)) return;

    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = instance->decoder.decode_count_bit;

    uint8_t n[11];
    subghz_protocol_lacrosse_tx_nibbles(instance->decoder.decode_data, n);
    instance->generic.serial = (uint32_t)((n[3] << 3) + (n[4] >> 1));
    instance->generic.btn = n[2]; /* message type */

    if(instance->base.callback)
        instance->base.callback(&instance->base, instance->base.context);
}

static void
    subghz_protocol_decoder_lacrosse_tx_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;

    if(level) {
        /* HIGH mark: PWM width encodes the bit (short=1, long=0). */
        if(duration < LACROSSE_MARK_MIN || duration >= LACROSSE_MARK_MAX) {
            /* Noise / out-of-range mark: abandon the current row. */
            instance->decoder.parser_step = LacrosseDecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            return;
        }
        if(instance->decoder.parser_step == LacrosseDecoderStepReset) {
            instance->decoder.parser_step = LacrosseDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        uint8_t bit = (duration < LACROSSE_MARK_MID) ? 1 : 0;
        subghz_protocol_blocks_add_bit(&instance->decoder, bit);
        if(instance->decoder.decode_count_bit > LACROSSE_MAX_BITS) {
            instance->decoder.parser_step = LacrosseDecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
    } else {
        /* Gap: a long gap ends the packet row; short gaps are inter-bit. */
        if(duration >= LACROSSE_GAP_RESET) {
            subghz_protocol_lacrosse_tx_finalize(instance);
            instance->decoder.parser_step = LacrosseDecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
    }
}

// ============================================================================
// alloc / free / hash / serialize
// ============================================================================

static void* subghz_protocol_decoder_lacrosse_tx_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLacrosseTx* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderLacrosseTx));
    instance->base.protocol = &subghz_protocol_lacrosse_tx;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void subghz_protocol_decoder_lacrosse_tx_free(void* context) {
    furi_assert(context);
    free(context);
}

static uint8_t subghz_protocol_decoder_lacrosse_tx_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_lacrosse_tx_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    subghz_protocol_decoder_lacrosse_tx_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_lacrosse_tx_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_lacrosse_tx_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderLacrosseTx* instance = context;

    uint8_t n[11];
    subghz_protocol_lacrosse_tx_nibbles(instance->generic.data, n);
    int sensor_id = (n[3] << 3) + (n[4] >> 1);
    uint8_t type = n[2];
    uint16_t raw = (uint16_t)((n[5] << 8) | (n[6] << 4) | n[7]);
    float value = n[5] * 10 + n[6] + n[7] * 0.1f;

    if(type == 0x00) {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%02X\r\n"
            "Temp:%.1f C",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            sensor_id,
            (double)(value - 50.0f));
    } else if(type == 0x0E) {
        if(raw != 0xff) {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Id:0x%02X\r\n"
                "Hum:%.0f%%",
                instance->generic.protocol_name,
                instance->generic.data_count_bit,
                sensor_id,
                (double)value);
        } else {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Id:0x%02X\r\n"
                "Hum:n/a",
                instance->generic.protocol_name,
                instance->generic.data_count_bit,
                sensor_id);
        }
    } else {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%02X Type:0x%X",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            sensor_id,
            type);
    }
}
