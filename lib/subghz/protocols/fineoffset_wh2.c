#include "fineoffset_wh2.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Fine Offset Electronics WH2 Temperature/Humidity sensor.
 *
 * Ported from rtl_433/src/devices/fineoffset.c (fineoffset_WH2_decode)
 *   Copyright (C) 2017 Tommy Vestermark, GPL-2+.
 *
 * RF profile (433.92 MHz, OOK):
 *   - Modulation: OOK_PULSE_PWM. Each data bit is one HIGH mark of variable
 *     width followed by a fixed ~1036us LOW gap:
 *       short mark ~544us  -> bit 1
 *       long  mark ~1524us -> bit 0   (rtl_433 nearest-neighbour slicer)
 *   - A gap longer than the reset limit (~1200us) ends the packet.
 *
 * Wire format (48 bits): [0xFF preamble][b0][b1][b2][b3][b4]
 *   b0: high nibble = type (always 0x4), low nibble = id[7:4]
 *   b1: high nibble = id[3:0], low nibble = temp[11:8]
 *   b2: temp[7:0]
 *   b3: humidity (0-100 %)
 *   b4: CRC-8, poly 0x31 (x^8+x^5+x^4+1), init 0x00, over b0..b3
 *
 *   temp is 12-bit signed magnitude, scaled by 10 (deg C).
 *
 * Receive/decode-only (this is a passive weather sensor); the encoder is a
 * NULL stub. The 48-bit frame is CRC-validated before the callback so noise
 * does not false-positive.
 */

#define TAG "FineOffsetWH2"

// Mark (HIGH pulse) classification windows, in microseconds.
#define WH2_MARK_MIN  200U // below this a mark is noise
#define WH2_MARK_MID  1024U // short (bit 1) below, long (bit 0) above
#define WH2_MARK_MAX  2200U // above this a mark is over-long -> abandon
#define WH2_GAP_RESET 1200U // a LOW gap >= this ends/aborts the packet

#define WH2_FRAME_BITS 48

static const SubGhzBlockConst subghz_protocol_fineoffset_wh2_const = {
    .te_short = 544,
    .te_long = 1524,
    .te_delta = 240,
    .min_count_bit_for_found = WH2_FRAME_BITS,
};

struct SubGhzProtocolDecoderFineOffsetWH2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

typedef struct SubGhzProtocolDecoderFineOffsetWH2 SubGhzProtocolDecoderFineOffsetWH2;

// ============================================================================
// Frame parse / validation (shared by decode + display)
// ============================================================================

typedef struct {
    uint8_t id;
    float temperature;
    uint8_t humidity; // 0xFF => no humidity
} FineOffsetWH2Frame;

/* Split a 48-bit frame held MSB-first in `data` into the 5 payload bytes. */
static void fineoffset_wh2_bytes(uint64_t data, uint8_t b[5]) {
    // b[0] = bits 32..39, ... b[4] = bits 0..7 (bits 40..47 are the 0xFF preamble)
    for(int i = 0; i < 5; i++) {
        b[i] = (uint8_t)((data >> (8 * (4 - i))) & 0xFF);
    }
}

/* Validate the 48-bit frame (preamble, type nibble, CRC). Returns true and
 * fills `out` when it is a genuine WH2 message. */
static bool fineoffset_wh2_parse(uint64_t data, FineOffsetWH2Frame* out) {
    if(((data >> 40) & 0xFF) != 0xFF) {
        return false; // preamble byte must be 0xFF
    }

    uint8_t b[5];
    fineoffset_wh2_bytes(data, b);

    // CRC-8 poly 0x31 init 0x00 over b[0..3]
    if(subghz_protocol_blocks_crc8(b, 4, 0x31, 0x00) != b[4]) {
        return false;
    }

    // Nibble 2 (type) must be 0x4
    if((b[0] >> 4) != 0x4) {
        return false;
    }

    out->id = (uint8_t)(((b[0] & 0x0F) << 4) | ((b[1] & 0xF0) >> 4));

    int temp = ((b[1] & 0x0F) << 8) | b[2]; // 12-bit signed magnitude
    if(temp & 0x800) {
        temp &= 0x7FF;
        temp = -temp;
    }
    out->temperature = (float)temp * 0.1f;

    out->humidity = b[3];
    return true;
}

// ============================================================================
// Decoder callbacks
// ============================================================================

static void* subghz_protocol_decoder_fineoffset_wh2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFineOffsetWH2* instance =
        malloc(sizeof(SubGhzProtocolDecoderFineOffsetWH2));
    instance->base.protocol = &subghz_protocol_fineoffset_wh2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void subghz_protocol_decoder_fineoffset_wh2_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_fineoffset_wh2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

static void subghz_protocol_fineoffset_wh2_emit(SubGhzProtocolDecoderFineOffsetWH2* instance) {
    uint64_t frame = instance->decoder.decode_data & 0xFFFFFFFFFFFFULL; // low 48 bits
    FineOffsetWH2Frame f;
    if(!fineoffset_wh2_parse(frame, &f)) {
        return;
    }
    instance->generic.data = frame;
    instance->generic.data_count_bit = WH2_FRAME_BITS;
    instance->generic.serial = f.id;

    FURI_LOG_D(TAG, "WH2 id:%02X t:%.1f h:%u", f.id, (double)f.temperature, f.humidity);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    // Require a fresh sync before emitting again.
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

/*
 * PWM pulse-duration state machine. A rolling 48-bit shift register self-syncs
 * on the 0xFF preamble; every time we hold >= 48 bits we test the low 48 bits
 * for a valid (preamble + type + CRC) frame.
 */
static void subghz_protocol_decoder_fineoffset_wh2_feed(
    void* context,
    bool level,
    uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;

    if(level) {
        // HIGH mark: classify by width into a data bit.
        uint8_t bit;
        if(duration < WH2_MARK_MIN || duration >= WH2_MARK_MAX) {
            // Noise / over-long: abandon the current accumulation.
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            return;
        } else if(duration < WH2_MARK_MID) {
            bit = 1; // short mark
        } else {
            bit = 0; // long mark
        }
        subghz_protocol_blocks_add_bit(&instance->decoder, bit);
        if(instance->decoder.decode_count_bit >= WH2_FRAME_BITS) {
            subghz_protocol_fineoffset_wh2_emit(instance);
        }
    } else {
        // LOW gap: only a reset-length gap is meaningful (ends the packet).
        if(duration >= WH2_GAP_RESET) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        // shorter gaps are the fixed inter-bit spacing: ignore
    }
}

static uint8_t subghz_protocol_decoder_fineoffset_wh2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_fineoffset_wh2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus subghz_protocol_decoder_fineoffset_wh2_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_fineoffset_wh2_const.min_count_bit_for_found);
}

static void
    subghz_protocol_decoder_fineoffset_wh2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFineOffsetWH2* instance = context;

    FineOffsetWH2Frame f = {0};
    bool ok = fineoffset_wh2_parse(instance->generic.data & 0xFFFFFFFFFFFFULL, &f);
    if(!ok) {
        f.id = (uint8_t)instance->generic.serial;
        f.humidity = 0xFF;
        f.temperature = 0.0f;
    }

    int t10 = (int)((f.temperature < 0 ? -f.temperature : f.temperature) * 10.0f + 0.5f);
    const char* sign = (f.temperature < 0) ? "-" : "";

    if(f.humidity != 0xFF) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%02X\r\n"
            "Temp:%s%d.%d C Hum:%d%%",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            f.id,
            sign,
            t10 / 10,
            t10 % 10,
            f.humidity);
    } else {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Id:0x%02X\r\n"
            "Temp:%s%d.%d C",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            f.id,
            sign,
            t10 / 10,
            t10 % 10);
    }
}

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_fineoffset_wh2_decoder = {
    .alloc = subghz_protocol_decoder_fineoffset_wh2_alloc,
    .free = subghz_protocol_decoder_fineoffset_wh2_free,
    .feed = subghz_protocol_decoder_fineoffset_wh2_feed,
    .reset = subghz_protocol_decoder_fineoffset_wh2_reset,
    .get_hash_data = subghz_protocol_decoder_fineoffset_wh2_get_hash_data,
    .serialize = subghz_protocol_decoder_fineoffset_wh2_serialize,
    .deserialize = subghz_protocol_decoder_fineoffset_wh2_deserialize,
    .get_string = subghz_protocol_decoder_fineoffset_wh2_get_string,
};

// Decode-only: NULL encoder (passive receive; no TX).
const SubGhzProtocolEncoder subghz_protocol_fineoffset_wh2_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_fineoffset_wh2 = {
    .name = FINEOFFSET_WH2_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Sensors,
    .decoder = &subghz_protocol_fineoffset_wh2_decoder,
    .encoder = &subghz_protocol_fineoffset_wh2_encoder,
};
