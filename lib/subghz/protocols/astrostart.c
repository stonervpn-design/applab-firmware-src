#include "astrostart.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * AstroStart 2000 — car remote start / keyfob decoder
 *
 * Ported from rtl_433 src/devices/astrostart_2000.c
 *
 * RF profile:
 *   - Carrier ~372.4 MHz (unusual band; tune manually — flagged 315/433 so it
 *     shows in the low-band frequency lists)
 *   - Modulation: OOK, Pulse Position Modulation (PPM)
 *   - Pulse (mark) roughly constant; the GAP after each pulse carries the bit:
 *       short gap ~326us  -> bit 0
 *       long  gap ~526us  -> bit 1
 *     rtl_433 slices at the short/long midpoint (426us) and ends the row at the
 *     reset limit (541us). Real inter-frame silences are far larger, so we widen
 *     the "1" window for jitter tolerance and only finalize on a clearly large
 *     gap.
 *
 * Frame: exactly 52 bits, MSB-first, packed as:
 *   bytes[0]      bits 0..7    button code
 *   bytes[1]      bits 8..15   ~button (bitwise inverse of bytes[0])
 *   bytes[2..5]   bits 16..47  32-bit remote ID (big-endian)
 *   bytes[6]>>4   bits 48..51  4-bit checksum
 *
 * Integrity (both must pass, else reject — kills false positives):
 *   1) bytes[0] == (~bytes[1] & 0xFF)
 *   2) checksum = sum of both nibbles of bytes[2..5], masked to 4 bits, each
 *      accumulation step masked to 4 bits, compared to bytes[6]>>4.
 */

#define TAG "Astrostart"

/* PPM gap timing (microseconds) */
#define ASTROSTART_TE_SHORT   326
#define ASTROSTART_TE_LONG    526
#define ASTROSTART_GAP_MID    426 /* (short+long)/2 : below = 0, above = 1 */
#define ASTROSTART_GAP_MIN    150 /* gaps shorter than this are noise */
#define ASTROSTART_GAP_MAX    750 /* gaps longer than this end the frame */
#define ASTROSTART_PULSE_MIN  120 /* sanity bounds on the mark/pulse width */
#define ASTROSTART_PULSE_MAX  900

#define ASTROSTART_FRAME_BITS 52

static const SubGhzBlockConst subghz_protocol_astrostart_const = {
    .te_short = ASTROSTART_TE_SHORT,
    .te_long = ASTROSTART_TE_LONG,
    .te_delta = 120,
    .min_count_bit_for_found = ASTROSTART_FRAME_BITS,
};

typedef enum {
    AstrostartDecoderStepReset = 0, /* waiting for a pulse to start */
    AstrostartDecoderStepCheckGap, /* got a pulse, expecting its gap */
} AstrostartDecoderStep;

struct SubGhzProtocolDecoderAstrostart {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};


// ============================================================================
// Encoder (emulate-only) — FIXED protocol, verbatim replay = permanent clone
//
// The stored 52-bit frame (button, ~button, 32-bit ID, 4-bit nibble-sum
// checksum) never changes for a given button, so re-emitting it byte-for-byte
// is a permanent clone. No counter, no keyed field — nothing to recompute.
//
// Waveform (matches the decoder's PPM slicer): each bit is a constant mark
// (TE_SHORT) followed by a gap that carries the bit — TE_SHORT (326us) = 0,
// TE_LONG (526us) = 1 (slicer midpoint 426us). A trailing mark plus an
// over-long inter-frame gap (>= GAP_MAX) makes the slicer finalize the 52nd
// bit before the burst repeats.
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE

struct SubGhzProtocolEncoderAstrostart {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

void* subghz_protocol_encoder_astrostart_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAstrostart* instance =
        malloc(sizeof(SubGhzProtocolEncoderAstrostart));
    instance->base.protocol = &subghz_protocol_astrostart;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 6;
    instance->encoder.size_upload = 128;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_astrostart_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAstrostart* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/* Build the LevelDuration[] burst for one 52-bit frame from generic.data. */
static bool
    subghz_protocol_encoder_astrostart_get_upload(SubGhzProtocolEncoderAstrostart* instance) {
    furi_assert(instance);
    size_t index = 0;
    /* per bit: mark + gap = 2 entries; plus a trailing mark + terminal gap */
    size_t size_upload = (instance->generic.data_count_bit * 2) + 2;
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Size upload exceeds allocated encoder buffer.");
        return false;
    }
    instance->encoder.size_upload = size_upload;

    /* Data bits, MSB-first: constant mark then a gap whose length carries the bit. */
    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)ASTROSTART_TE_SHORT);
        if(bit_read(instance->generic.data, i - 1)) {
            /* bit 1 -> long gap */
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)ASTROSTART_TE_LONG);
        } else {
            /* bit 0 -> short gap */
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)ASTROSTART_TE_SHORT);
        }
    }
    /* Trailing mark so the last data gap is a real bit, then an over-long gap
     * (>= ASTROSTART_GAP_MAX) so the slicer finalizes all 52 bits. */
    instance->encoder.upload[index++] =
        level_duration_make(true, (uint32_t)ASTROSTART_TE_SHORT);
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)(ASTROSTART_GAP_MAX * 2));

    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_astrostart_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderAstrostart* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(instance->generic.data_count_bit != ASTROSTART_FRAME_BITS) {
            FURI_LOG_E(TAG, "Wrong number of bits in key");
            ret = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
        // Optional value
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        if(!subghz_protocol_encoder_astrostart_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_astrostart_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAstrostart* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_astrostart_yield(void* context) {
    SubGhzProtocolEncoderAstrostart* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

#endif // ENABLE_EMULATE_FEATURE

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_astrostart_decoder = {
    .alloc = subghz_protocol_decoder_astrostart_alloc,
    .free = subghz_protocol_decoder_astrostart_free,
    .feed = subghz_protocol_decoder_astrostart_feed,
    .reset = subghz_protocol_decoder_astrostart_reset,
    .get_hash_data = subghz_protocol_decoder_astrostart_get_hash_data,
    .serialize = subghz_protocol_decoder_astrostart_serialize,
    .deserialize = subghz_protocol_decoder_astrostart_deserialize,
    .get_string = subghz_protocol_decoder_astrostart_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_astrostart_encoder = {
    // FIXED protocol: verbatim replay of the stored 52-bit frame = permanent clone.
    .alloc = subghz_protocol_encoder_astrostart_alloc,
    .free = subghz_protocol_encoder_astrostart_free,
    .deserialize = subghz_protocol_encoder_astrostart_deserialize,
    .stop = subghz_protocol_encoder_astrostart_stop,
    .yield = subghz_protocol_encoder_astrostart_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_astrostart_encoder = {
    // Decode-only when emulate is disabled. NULL ops, no stub funcs.
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_astrostart = {
    .name = ASTROSTART_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_astrostart_decoder,
    .encoder = &subghz_protocol_astrostart_encoder,
};

// ============================================================================
// Frame validation (rtl_433 parity)
// ============================================================================

/* Returns true and fills serial/btn/crc when the 52-bit frame passes both the
 * inverse-button check and the 4-bit checksum. */
static bool subghz_protocol_astrostart_check(
    uint64_t data,
    uint32_t* serial,
    uint8_t* btn,
    uint8_t* crc) {
    uint8_t bytes[7];
    bytes[0] = (uint8_t)(data >> 44); /* button          bits 0..7   */
    bytes[1] = (uint8_t)(data >> 36); /* ~button         bits 8..15  */
    bytes[2] = (uint8_t)(data >> 28); /* id[31..24]      bits 16..23 */
    bytes[3] = (uint8_t)(data >> 20); /* id[23..16]      bits 24..31 */
    bytes[4] = (uint8_t)(data >> 12); /* id[15..8]       bits 32..39 */
    bytes[5] = (uint8_t)(data >> 4); /* id[7..0]        bits 40..47 */
    uint8_t rx_checksum = (uint8_t)(data & 0x0F); /* checksum bits 48..51 */

    /* 1) button must be the bitwise inverse of the next byte */
    if(bytes[0] != (uint8_t)(~bytes[1] & 0xFF)) {
        return false;
    }

    /* 2) nibble-sum checksum over the four ID bytes */
    uint8_t expected = 0;
    for(int i = 2; i < 6; i++) {
        expected = (expected + (bytes[i] >> 4)) & 0x0F;
        expected = (expected + bytes[i]) & 0x0F;
    }
    if(expected != rx_checksum) {
        return false;
    }

    *serial = ((uint32_t)bytes[2] << 24) | ((uint32_t)bytes[3] << 16) |
              ((uint32_t)bytes[4] << 8) | (uint32_t)bytes[5];
    *btn = bytes[0];
    *crc = rx_checksum;
    return true;
}

/* Human-readable button name (matches the rtl_433 button_map). Rolling set of
 * six alternates per button plus a "Multiple" catch-all. */
static const char* subghz_protocol_astrostart_btn_name(uint8_t btn) {
    static const struct {
        const char* name;
        uint8_t len;
        uint8_t vals[6];
    } map[] = {
        {"Lock", 6, {0x2b, 0x03, 0x27, 0x0f, 0x35, 0x37}},
        {"Panic", 6, {0x1f, 0x35, 0x0d, 0x25, 0x15, 0x2d}},
        {"Start", 6, {0x13, 0x37, 0x2d, 0x33, 0x3d, 0x3b}},
        {"Stop", 6, {0x2f, 0x0d, 0x33, 0x03, 0x1d, 0x17}},
        {"Trunk", 6, {0x23, 0x25, 0x3d, 0x1d, 0x27, 0x07}},
        {"Unlock", 6, {0x0b, 0x15, 0x3b, 0x17, 0x07, 0x0f}},
        {"Multiple", 1, {0x3f, 0, 0, 0, 0, 0}},
    };
    for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        for(uint8_t j = 0; j < map[i].len; j++) {
            if(btn == map[i].vals[j]) return map[i].name;
        }
    }
    return "?";
}

static void subghz_protocol_astrostart_finalize(SubGhzProtocolDecoderAstrostart* instance) {
    if(instance->decoder.decode_count_bit != ASTROSTART_FRAME_BITS) return;

    uint32_t serial = 0;
    uint8_t btn = 0;
    uint8_t crc = 0;
    if(!subghz_protocol_astrostart_check(
           instance->decoder.decode_data, &serial, &btn, &crc)) {
        return;
    }

    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = ASTROSTART_FRAME_BITS;
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = 0; /* no rolling counter in this protocol */
    instance->generic.crc = crc;

    FURI_LOG_D(
        TAG, "Frame ID:%08lX Btn:%02X CRC:%X", (unsigned long)serial, btn, crc);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}







// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_astrostart_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAstrostart* instance =
        malloc(sizeof(SubGhzProtocolDecoderAstrostart));
    instance->base.protocol = &subghz_protocol_astrostart;
    instance->generic.protocol_name = subghz_protocol_astrostart.name;
    return instance;
}

void subghz_protocol_decoder_astrostart_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_astrostart_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;
    instance->decoder.parser_step = AstrostartDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

// ============================================================================
// Decoder: feed — PPM slicer over LevelDuration pulses
//
// Marks and spaces strictly alternate. A mark (level=true) is the pulse; the
// space (level=false) that follows carries one bit by its length. A large space
// (>= GAP_MAX) or an out-of-range mark ends the current frame.
// ============================================================================

void subghz_protocol_decoder_astrostart_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;

    switch(instance->decoder.parser_step) {
    case AstrostartDecoderStepReset:
        if(level && duration >= ASTROSTART_PULSE_MIN && duration <= ASTROSTART_PULSE_MAX) {
            /* Valid leading pulse — begin collecting a fresh frame */
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = AstrostartDecoderStepCheckGap;
        }
        break;

    case AstrostartDecoderStepCheckGap:
        if(level) {
            /* Another pulse — validate its width, otherwise drop back to reset */
            if(duration < ASTROSTART_PULSE_MIN || duration > ASTROSTART_PULSE_MAX) {
                subghz_protocol_astrostart_finalize(instance);
                instance->decoder.parser_step = AstrostartDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            }
        } else {
            /* Gap — classify into a bit or finalize the frame */
            if(duration < ASTROSTART_GAP_MIN) {
                /* Runt gap: noise, abandon frame */
                instance->decoder.parser_step = AstrostartDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else if(duration >= ASTROSTART_GAP_MAX) {
                /* End of frame */
                subghz_protocol_astrostart_finalize(instance);
                instance->decoder.parser_step = AstrostartDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else {
                /* Data gap: short = 0, long = 1 */
                uint8_t bit = (duration < ASTROSTART_GAP_MID) ? 0 : 1;
                if(instance->decoder.decode_count_bit >= 64) {
                    /* Overflow guard — cannot be a valid 52-bit frame anymore */
                    instance->decoder.parser_step = AstrostartDecoderStepReset;
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 0;
                } else {
                    subghz_protocol_blocks_add_bit(&instance->decoder, bit);
                }
            }
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / string
// ============================================================================

uint8_t subghz_protocol_decoder_astrostart_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_astrostart_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_astrostart_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_astrostart_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_astrostart_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAstrostart* instance = context;

    /* Recover fields for display (also covers loaded/deserialized frames). */
    uint32_t serial = instance->generic.serial;
    uint8_t btn = instance->generic.btn;
    uint8_t crc = instance->generic.crc;
    subghz_protocol_astrostart_check(instance->generic.data, &serial, &btn, &crc);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%013llX\r\n"
        "ID:%08lX\r\n"
        "Btn:%02X (%s)  CRC:%X",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)instance->generic.data,
        (unsigned long)serial,
        btn,
        subghz_protocol_astrostart_btn_name(btn),
        crc);
}
