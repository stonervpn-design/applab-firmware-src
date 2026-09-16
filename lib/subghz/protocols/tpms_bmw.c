#include "tpms_bmw.h"
#include "../blocks/generic.h"

#define TAG "TPMSBmw"

// Port of rtl_433/src/devices/tpms_bmw.c
//   (C) 2024 Bruno OCTAU (ProfBoc75), @petrjac, @Gucioo, Christian W. Zuckschwerdt, GPL-2+.
//
// BMW Gen4/Gen5 TPMS and Audi TPMS "Pressure Alert" sensor.
// FSK, PCM (NRZ) raw bits at 25 us, Manchester-encoded (G.E. Thomas), then the
// bit stream is inverted ("MC zero-bit coded"). Net effect: the decoded data bit
// equals the FIRST half-bit of each raw Manchester pair (the two half-bits of a
// valid pair always differ).
//
// Raw preamble (pre-Manchester NRZ): 0xAA59. We also accept the RF-inverted
// preamble 0x55A6 and then invert every raw bit, so either FSK deviation sign
// decodes.
//
// Wire format after Manchester+invert:
//   BMW  (11 bytes): MM II II II II PP TT F1 F2 F3 CC
//   Audi ( 8 bytes): MM II II II II PP TT CC
//     MM          = brand id (0x00 Audi-Alert, 0x03 HUF/Beru, 0x23 Schrader, 0x80 Conti, 0x88 Audi)
//     II II II II = 32-bit sensor id
//     PP          = pressure, kPa = PP * 2.45
//     TT          = temperature, C = TT - 52
//     CC          = CRC-8 poly 0x2F init 0xAA over all preceding bytes (residue
//                   over the whole frame including CRC == 0).
//
// Frame length is discriminated the way rtl_433 does it: an 11-byte BMW frame is
// Manchester-continuous to 88 bits; an 8-byte Audi frame stops after 64 bits, so
// its terminating gap/coding-break with exactly 64 collected bits marks Audi.
//
// Decode-only port (encoder all-NULL, like audiovox.c).

#define BMW_PREAMBLE     0xAA59U
#define BMW_PREAMBLE_INV 0x55A6U // (uint16_t)~0xAA59
#define BMW_AUDI_BITS    64 // 8 bytes
#define BMW_FULL_BITS    88 // 11 bytes

static const SubGhzBlockConst tpms_protocol_bmw_const = {
    .te_short = 25, // one raw PCM bit
    .te_long = 50, // two merged raw bits
    .te_delta = 12,
    .min_count_bit_for_found = BMW_AUDI_BITS,
};

typedef enum {
    BmwStepReset = 0, // hunting raw preamble
    BmwStepData, // collecting Manchester-decoded data bits
} BmwStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    uint16_t header_shift; // rolling raw-bit register for preamble search
    bool invert; // RF polarity chosen by which preamble matched
    bool half_have; // holding the first half-bit of a Manchester pair
    uint8_t half_bit; // stored first half-bit
    uint8_t len_bytes; // resolved frame length for the matched CRC (8 or 11)
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderBmw;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderBmw;

const SubGhzProtocolDecoder tpms_protocol_bmw_decoder = {
    .alloc = tpms_protocol_decoder_bmw_alloc,
    .free = tpms_protocol_decoder_bmw_free,
    .feed = tpms_protocol_decoder_bmw_feed,
    .reset = tpms_protocol_decoder_bmw_reset,
    .get_hash_data = tpms_protocol_decoder_bmw_get_hash_data,
    .serialize = tpms_protocol_decoder_bmw_serialize,
    .deserialize = tpms_protocol_decoder_bmw_deserialize,
    .get_string = tpms_protocol_decoder_bmw_get_string,
};

// Encoder (TX). Mirrors tpms_citroen: raw sync preamble in the half-symbol stream,
// then the payload as G.E. Thomas Manchester. Definitions live at the bottom.
void* tpms_protocol_encoder_bmw_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_bmw_free(void* context);
void tpms_protocol_encoder_bmw_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_bmw_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_bmw_yield(void* context);

const SubGhzProtocolEncoder tpms_protocol_bmw_encoder = {
    .alloc = tpms_protocol_encoder_bmw_alloc,
    .free = tpms_protocol_encoder_bmw_free,
    .deserialize = tpms_protocol_encoder_bmw_deserialize,
    .stop = tpms_protocol_encoder_bmw_stop,
    .yield = tpms_protocol_encoder_bmw_yield,
};

const SubGhzProtocol tpms_protocol_bmw = {
    .name = TPMS_PROTOCOL_BMW_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_bmw_decoder,
    .encoder = &tpms_protocol_bmw_encoder,
};

void* tpms_protocol_decoder_bmw_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderBmw* instance = calloc(1, sizeof(TPMSProtocolDecoderBmw));
    instance->base.protocol = &tpms_protocol_bmw;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_bmw_free(void* context) {
    furi_assert(context);
    free(context);
}

static void bmw_reset_state(TPMSProtocolDecoderBmw* instance) {
    instance->decoder.parser_step = BmwStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->invert = false;
    instance->half_have = false;
    instance->half_bit = 0;
    instance->head_64_bit = 0;
}

void tpms_protocol_decoder_bmw_reset(void* context) {
    furi_assert(context);
    bmw_reset_state(context);
}

// Extract a `count`-bit frame (count a multiple of 8) into b[], MSB-first. The
// top (count-64) bits live in head_64_bit, the low 64 bits in decode_data.
static void bmw_get_bytes(TPMSProtocolDecoderBmw* instance, uint16_t count, uint8_t b[11]) {
    int head_bytes = (count > 64) ? (int)((count - 64) / 8) : 0;
    for(int i = 0; i < head_bytes; i++) {
        b[i] = (uint8_t)((instance->head_64_bit >> (8 * (head_bytes - 1 - i))) & 0xFF);
    }
    for(int i = 0; i < 8; i++) {
        b[head_bytes + i] = (uint8_t)((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF);
    }
}

static void bmw_analyze(TPMSProtocolDecoderBmw* instance, const uint8_t* b) {
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    instance->generic.pressure = (float)b[5] * 2.45f * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int)b[6] - 52);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Try to close out an Audi (64-bit / 8-byte) frame, then fully reset. Called
// when the Manchester stream breaks (coding violation or inter-frame gap).
static void bmw_finish_or_reset(TPMSProtocolDecoderBmw* instance) {
    if(instance->decoder.parser_step == BmwStepData &&
       instance->decoder.decode_count_bit == BMW_AUDI_BITS) {
        uint8_t b[11];
        bmw_get_bytes(instance, BMW_AUDI_BITS, b);
        if(subghz_protocol_blocks_crc8(b, 8, 0x2F, 0xAA) == 0) {
            instance->len_bytes = 8;
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            bmw_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
    }
    bmw_reset_state(instance);
}

static void bmw_enter_data(TPMSProtocolDecoderBmw* instance, bool invert) {
    instance->decoder.parser_step = BmwStepData;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->half_have = false;
    instance->half_bit = 0;
    instance->invert = invert;
}

static void bmw_process_raw_bit(TPMSProtocolDecoderBmw* instance, uint8_t raw) {
    if(instance->decoder.parser_step == BmwStepReset) {
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (raw & 1));
        if(instance->header_shift == BMW_PREAMBLE) {
            bmw_enter_data(instance, false);
        } else if(instance->header_shift == BMW_PREAMBLE_INV) {
            bmw_enter_data(instance, true);
        }
        return;
    }

    // BmwStepData: Manchester pairs -> decoded bit = first (polarity-corrected) half.
    uint8_t r = (uint8_t)((raw & 1) ^ (instance->invert ? 1 : 0));
    if(!instance->half_have) {
        instance->half_bit = r;
        instance->half_have = true;
        return;
    }
    instance->half_have = false;
    if(r == instance->half_bit) {
        // Both half-bits equal -> Manchester coding violation = frame boundary.
        bmw_finish_or_reset(instance);
        return;
    }
    subghz_protocol_blocks_add_to_128_bit(
        &instance->decoder, instance->half_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == BMW_FULL_BITS) {
        uint8_t b[11];
        bmw_get_bytes(instance, BMW_FULL_BITS, b);
        if(subghz_protocol_blocks_crc8(b, 11, 0x2F, 0xAA) == 0) {
            instance->len_bytes = 11;
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            bmw_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        bmw_reset_state(instance);
    }
}

void tpms_protocol_decoder_bmw_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;

    const uint32_t te = tpms_protocol_bmw_const.te_short;
    // Number of raw PCM bits in this edge (runs are 1 or 2 te in valid Manchester).
    uint32_t count = (duration + te / 2) / te;
    if(count < 1 || count > 2 ||
       DURATION_DIFF(duration, count * te) > tpms_protocol_bmw_const.te_delta) {
        // Gap / noise / out-of-spec run -> Manchester stream broken.
        bmw_finish_or_reset(instance);
        return;
    }

    uint8_t raw = level ? 1 : 0;
    for(uint32_t i = 0; i < count; i++) {
        bmw_process_raw_bit(instance, raw);
    }
}

uint8_t tpms_protocol_decoder_bmw_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_bmw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    // Accept either frame length (Audi 64-bit or BMW 88-bit); decode-only.
    return tpms_block_generic_deserialize(&instance->generic, flipper_format);
}

void tpms_protocol_decoder_bmw_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer + Encoder (TX)
// -----------------------------------------------------------------------------
//
// We transmit the BMW/Conti FULL 88-bit (11-byte) frame - the decoder auto-fires
// on 88 collected bits (no terminating gap required) and validates the 11-byte
// CRC. Wire format: MM II II II II PP TT F1 F2 F3 CC.
//   MM          = brand id. Not exposed by the decoder (only the CRC covers it),
//                 so we emit a fixed 0x80 (Conti).
//   II II II II = 32-bit sensor id (generic.id)
//   PP          = pressure raw, bar = PP * 2.45 * 0.01  =>  PP = bar / 0.0245
//   TT          = temperature raw, C = TT - 52          =>  TT = C + 52
//   F1 F2 F3    = flags (crafted as 0 here; preserved from generic.data on replay)
//   CC          = CRC-8 poly 0x2F init 0xAA over b[0..9] (residue over all 11 == 0)
//
// generic.data carries the low 64 bits of the 88-bit frame = b[3..10]
// (b[3],b[4] = low 16 id bits; b[5]=PP; b[6]=TT; b[7..9]=flags; b[10]=CRC). The
// decoder stores exactly these low 64 bits into generic.data, so pack/build_frame
// are the mirror of bmw_analyze()/bmw_get_bytes().
//
// Manchester: G.E. Thomas (like tpms_citroen). The decoded data bit equals the
// FIRST half-symbol of each transitioning pair (invert=false for the 0xAA59
// preamble), so bit 1 => raw high-then-low, bit 0 => raw low-then-high.
//
// Worst-case entries: 1 gap + 16 raw preamble + 2*88 data + 1 tail = 194.
#define TPMS_BMW_ENCODER_UPLOAD_SIZE 260
#define BMW_FULL_BYTES               11

void tpms_protocol_bmw_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar = PP * 2.45 * 0.01  ->  PP = bar / 0.0245
    int p_raw = (int)(g->pressure / 0.0245f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // temperature: C = TT - 52  ->  TT = C + 52
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 52;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    // generic.data holds b[3..10]. b[3],b[4] are the low 16 id bits; the encoder's
    // build_frame recovers b[1],b[2] from generic.id and b[0]=MM, then recomputes
    // b[10]=CRC. Only b[5]=PP, b[6]=TT are (re)written from the packed values;
    // b[7..9] flags are preserved on replay (crafted 0 from scratch).
    uint8_t b[8]; // = frame b[3..10]
    b[0] = (uint8_t)((g->id >> 8) & 0xFF); // b[3]
    b[1] = (uint8_t)(g->id & 0xFF); // b[4]
    b[2] = (uint8_t)p_raw; // b[5]
    b[3] = (uint8_t)t; // b[6]
    b[4] = 0; // b[7] F1
    b[5] = 0; // b[8] F2
    b[6] = 0; // b[9] F3
    b[7] = 0; // b[10] CRC (recomputed in build_frame)
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = BMW_FULL_BITS;
}

void* tpms_protocol_encoder_bmw_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderBmw* instance = malloc(sizeof(TPMSProtocolEncoderBmw));
    instance->base.protocol = &tpms_protocol_bmw;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_BMW_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_bmw_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderBmw* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_bmw_stop(void* context) {
    TPMSProtocolEncoderBmw* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 11-byte frame from generic.id + generic.data and recompute the
// CRC so a modified / 0-PSI frame is always valid.
static void
    tpms_protocol_encoder_bmw_build_frame(TPMSBlockGeneric* generic, uint8_t b[BMW_FULL_BYTES]) {
    b[0] = 0x80; // MM brand (Conti); decoder only CRC-checks it
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    b[2] = (uint8_t)((generic->id >> 16) & 0xFF);
    // b[3..10] = low 64 bits of the frame, held in generic.data (MSB-first)
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    // CRC-8 poly 0x2F init 0xAA over b[0..9]; appending it makes residue over 11 == 0
    b[10] = subghz_protocol_blocks_crc8(b, 10, 0x2F, 0xAA);
}

static bool tpms_protocol_encoder_bmw_get_upload(TPMSProtocolEncoderBmw* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_bmw_const.te_short;

    uint8_t frame[BMW_FULL_BYTES];
    tpms_protocol_encoder_bmw_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                  \
    do {                                                                \
        if(last_duration != 0 && last_level == (lvl)) {                 \
            last_duration += (dur);                                     \
        } else {                                                        \
            if(last_duration != 0) {                                    \
                if(index >= max) return false;                          \
                instance->encoder.upload[index++] =                     \
                    level_duration_make(last_level, last_duration);     \
            }                                                           \
            last_level = (lvl);                                         \
            last_duration = (dur);                                      \
        }                                                               \
    } while(0)

// G.E. Thomas Manchester: data bit = FIRST half-symbol (invert=false for 0xAA59).
//   bit 1 -> raw high then low;  bit 0 -> raw low then high.
#define MAN_BIT(bit)         \
    do {                     \
        if(bit) {            \
            EMIT(true, te);  \
            EMIT(false, te); \
        } else {             \
            EMIT(false, te); \
            EMIT(true, te);  \
        }                    \
    } while(0)

    // Inter-frame gap (also forces the decoder to a clean Reset state).
    EMIT(false, te * 50);

    // Raw sync preamble 0xAA59 - NOT Manchester. Each preamble bit is ONE raw
    // half-symbol; the decoder hunts this 16-bit pattern in the raw bit stream.
    static const uint8_t preamble[2] = {0xAA, 0x59};
    for(int i = 0; i < 2; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 88 data bits, MSB first (frame[0]..frame[10]), Manchester-encoded.
    for(int i = 0; i < BMW_FULL_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        MAN_BIT((frame[byte] >> bit) & 0x1);
    }

    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef MAN_BIT
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    tpms_protocol_encoder_bmw_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderBmw* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        // FULL frame is 88 bits (min_count_bit_for_found is the 64-bit Audi floor).
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, BMW_FULL_BITS);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_BMW_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_bmw_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_bmw_yield(void* context) {
    TPMSProtocolEncoderBmw* instance = context;

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
