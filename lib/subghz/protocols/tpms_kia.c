#include "tpms_kia.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "TPMSKia"

// Port of rtl_433/src/devices/tpms_kia.c
//   (C) 2022 Lasse Mikkel Reinhold, Todor Uzunov aka teou, TTiges,
//       2019 Andreas Spiess, 2017 Christian W. Zuckschwerdt, GPL-2+
//
// Kia Rio UB III (2011-2017) and some Hyundai models.
//
// Modulation: FSK, PCM at 50 us/bit (rtl_433 FSK_PULSE_PCM, short=long=50,
//   reset_limit=200). The frame is 154 raw bits: a 16-bit raw sync 0xED71 that
//   is NOT Manchester-coded, followed by 138 Manchester half-symbols that decode
//   to 69 payload bits (b[0..8], where b[8] carries only its top 5 bits).
//
// Payload bit layout (leftmost bit of each field is most significant), matching
// rtl_433 exactly (nibble-packed):
//   aaaa pppppppp tttttttt iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii dddddddddddd ccccc
//     a: 4-bit unknown1 (0xF in all known readings)   -> b[0] high nibble
//     p: 8-bit pressure = PSI * 5                       -> b[0]<<4 | b[1]>>4
//     t: 8-bit temperature = Celsius + 50               -> b[1]<<4 | b[2]>>4
//     i: 32-bit sensor id     -> b[2]<<28|b[3]<<20|b[4]<<12|b[5]<<4|b[6]>>4
//     d: 12-bit unknown2 (varies per packet)            -> b[6]<<8 | b[7] (low 12)
//     c: 5 stored CRC bits    -> b[8] high 5 bits; the low 3 CRC bits fall past
//        the 154-bit packet and are treated as 000.
//   crc = crc8(b[0..7], poly=0x07, init=0x76); the check is
//        (b[8] & ~0x7) == crc8(...), i.e. the true CRC's low 3 bits must be 0.
//
// pressure(bar) = (raw / 5) / 14.50377   temperature(C) = raw - 50

#define KIA_PREAMBLE 0xED71U
#define KIA_DATA_BITS 69 // decoded payload bits (b[0..7] full + b[8] top 5)
#define KIA_PSI_PER_BAR 14.5037738f

static const SubGhzBlockConst tpms_protocol_kia_const = {
    .te_short = 50,
    .te_long = 50,
    .te_delta = 15,
    .min_count_bit_for_found = KIA_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // FSK where the frame is RAW half-symbols: a fixed 16-bit sync (0xED71, or
    // its inverse for flipped FSK polarity) in the raw bits, then a 69-bit
    // Manchester-coded payload (matches rtl_433 tpms_kia: search 0xED71 in raw
    // bits, then manchester-decode only the data).
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting Manchester data
    bool data_second; // pair convention: false=data is 1st half (0xED71), true=2nd half (inverse)
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint8_t packet[9]; // decoded payload bytes b[0..8]
    uint16_t data_bits; // number of decoded payload bits collected
} TPMSProtocolDecoderKia;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderKia;

const SubGhzProtocolDecoder tpms_protocol_tpms_kia_decoder = {
    .alloc = tpms_protocol_decoder_tpms_kia_alloc,
    .free = tpms_protocol_decoder_tpms_kia_free,
    .feed = tpms_protocol_decoder_tpms_kia_feed,
    .reset = tpms_protocol_decoder_tpms_kia_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_kia_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_kia_serialize,
    .deserialize = tpms_protocol_decoder_tpms_kia_deserialize,
    .get_string = tpms_protocol_decoder_tpms_kia_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_kia_encoder = {
    .alloc = tpms_protocol_encoder_tpms_kia_alloc,
    .free = tpms_protocol_encoder_tpms_kia_free,
    .deserialize = tpms_protocol_encoder_tpms_kia_deserialize,
    .stop = tpms_protocol_encoder_tpms_kia_stop,
    .yield = tpms_protocol_encoder_tpms_kia_yield,
};

const SubGhzProtocol tpms_protocol_tpms_kia = {
    .name = TPMS_PROTOCOL_KIA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_kia_decoder,
    .encoder = &tpms_protocol_tpms_kia_encoder,
};

// rtl_433 crc8(): MSB-first, no reflection. poly/init passed in.
static uint8_t tpms_kia_crc8(const uint8_t* message, unsigned n, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    for(unsigned byte = 0; byte < n; byte++) {
        crc ^= message[byte];
        for(int bit = 0; bit < 8; bit++) {
            if(crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ poly);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

void* tpms_protocol_decoder_tpms_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderKia* instance = calloc(1, sizeof(TPMSProtocolDecoderKia));
    instance->base.protocol = &tpms_protocol_tpms_kia;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_kia_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_kia_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->data_bits = 0;
    memset(instance->packet, 0, sizeof(instance->packet));
}

static bool tpms_kia_check_crc(const uint8_t b[9]) {
    // rtl_433: crc = b[8] & ~0x7; c = crc8(b, 8, 0x07, 0x76); pass iff crc == c.
    uint8_t crc = (uint8_t)(b[8] & (uint8_t)~0x7);
    uint8_t c = tpms_kia_crc8(b, 8, 0x07, 0x76);
    return crc == c;
}

static void tpms_kia_analyze(TPMSProtocolDecoderKia* instance) {
    const uint8_t* b = instance->packet;
    uint8_t pressure = (uint8_t)((b[0] << 4) | (b[1] >> 4));
    uint8_t temperature = (uint8_t)((b[1] << 4) | (b[2] >> 4));
    instance->generic.id = ((uint32_t)(b[2] & 0x0F) << 28) | ((uint32_t)b[3] << 20) |
                           ((uint32_t)b[4] << 12) | ((uint32_t)b[5] << 4) | (uint32_t)(b[6] >> 4);
    // rtl_433 pressure = PSI * 5. Store as BAR to match TPMSBlockGeneric.
    instance->generic.pressure = ((float)pressure / 5.0f) / KIA_PSI_PER_BAR;
    instance->generic.temperature = (float)((int)temperature - 50);
    instance->generic.battery_low = TPMS_NO_BATT; // Kia frame carries no battery bit

    // Publish the low 64 bits (b[0..7]) as generic.data for save/load + hashing.
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    instance->generic.data = data;
    instance->generic.data_count_bit = KIA_DATA_BITS;
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = KIA_DATA_BITS;
}

void tpms_protocol_tpms_kia_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure(bar) -> PSI -> raw = PSI * 5 (rtl_433 raw scaling).
    float psi = g->pressure * KIA_PSI_PER_BAR;
    int p_raw = (int)(psi * 5.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t_raw = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // C + 50
    if(t_raw < 0) t_raw = 0;
    if(t_raw > 0xFF) t_raw = 0xFF;

    uint32_t id = g->id;
    uint8_t b[9];
    b[0] = (uint8_t)(0xF0 | ((p_raw >> 4) & 0x0F)); // unknown1=0xF, pressure hi nibble
    b[1] = (uint8_t)(((p_raw & 0x0F) << 4) | ((t_raw >> 4) & 0x0F));
    b[2] = (uint8_t)(((t_raw & 0x0F) << 4) | ((id >> 28) & 0x0F));
    b[3] = (uint8_t)((id >> 20) & 0xFF);
    b[4] = (uint8_t)((id >> 12) & 0xFF);
    b[5] = (uint8_t)((id >> 4) & 0xFF);
    // b[6] high nibble = id low nibble; low nibble = unknown2 high nibble (start 0).
    // b[7] = unknown2 low byte — a free field we search so the CRC-8 low 3 bits
    // become 0, which the on-wire 5-bit CRC field requires.
    b[6] = (uint8_t)(((id & 0x0F) << 4));
    b[7] = 0x00;
    b[8] = 0x00;
    for(int v = 0; v < 256; v++) {
        b[7] = (uint8_t)v;
        uint8_t c = tpms_kia_crc8(b, 8, 0x07, 0x76);
        if((c & 0x07) == 0) {
            b[8] = c;
            break;
        }
    }

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = KIA_DATA_BITS;
}

// Consume one raw half-symbol bit: hunt the raw 16-bit preamble (0xED71, or the
// inverted-polarity ~0xED71), then Manchester-decode the 69-bit payload.
static void tpms_kia_emit_raw_bit(TPMSProtocolDecoderKia* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == KIA_PREAMBLE ||
           instance->raw_shift == (uint16_t)(~KIA_PREAMBLE)) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == (uint16_t)(~KIA_PREAMBLE));
            instance->have_half = false;
            instance->data_bits = 0;
            memset(instance->packet, 0, sizeof(instance->packet));
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (0xED71) or 2nd
    // half (inverted). No transition = end/garbage -> resync on the preamble.
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->data_second ? bit : instance->first_half;
    if(instance->data_bits < KIA_DATA_BITS) {
        if(data_bit) {
            instance->packet[instance->data_bits >> 3] |=
                (uint8_t)(1u << (7 - (instance->data_bits & 7)));
        }
        instance->data_bits++;
    }
    if(instance->data_bits == KIA_DATA_BITS) {
        if(tpms_kia_check_crc(instance->packet)) {
            tpms_kia_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_tpms_kia_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;

    const uint32_t te = tpms_protocol_kia_const.te_short; // 50us half-symbol
    // A gap (>= 4 half-symbols; rtl_433 reset_limit=200 = 4*50) ends the burst.
    if(duration >= te * 4) {
        tpms_protocol_decoder_tpms_kia_reset(instance);
        return;
    }
    // Expand this pulse into 1..3 raw half-symbol bits (the raw preamble contains
    // runs of up to three equal bits; Manchester data never exceeds two).
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 3) n = 3;
    for(uint32_t k = 0; k < n; k++) {
        tpms_kia_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_kia_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_kia_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_kia_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderKia* instance = context;
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
// Encoder
// -----------------------------------------------------------------------------
//
// FSK: raw 16-bit sync 0xED71 (one raw half-symbol per bit, NOT Manchester),
// then 69 Manchester-II data bits, non-inverting:
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
// which is the 0xED71 convention the decoder uses (data = 1st half of each pair).
//
// generic.data holds the low 64 bits b[0..7]; b[8] (the 5-bit CRC, low 3 zero) is
// recomputed from b[0..7] in build_frame, so the wire CRC always matches.
//
// Worst case: 1 gap + 16 preamble + 2*69 data + 1 flush = 156.
#define TPMS_KIA_ENCODER_UPLOAD_SIZE 320

void* tpms_protocol_encoder_tpms_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderKia* instance = malloc(sizeof(TPMSProtocolEncoderKia));
    instance->base.protocol = &tpms_protocol_tpms_kia;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_KIA_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_kia_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderKia* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_kia_stop(void* context) {
    TPMSProtocolEncoderKia* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 9-byte frame: b[0..7] from generic.data, b[8] = recomputed
// CRC-8 (poly 0x07, init 0x76) — its low 3 bits are zero for packed frames.
static void tpms_protocol_encoder_tpms_kia_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[8] = tpms_kia_crc8(b, 8, 0x07, 0x76);
}

static bool tpms_protocol_encoder_tpms_kia_get_upload(TPMSProtocolEncoderKia* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_kia_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_tpms_kia_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                                \
    do {                                                                              \
        if(last_duration != 0 && last_level == (lvl)) {                               \
            last_duration += (dur);                                                   \
        } else {                                                                      \
            if(last_duration != 0) {                                                  \
                if(index >= max) return false;                                        \
                instance->encoder.upload[index++] =                                   \
                    level_duration_make(last_level, last_duration);                   \
            }                                                                         \
            last_level = (lvl);                                                       \
            last_duration = (dur);                                                    \
        }                                                                             \
    } while(0)

// Manchester data bit: 1st half = the data bit, 2nd half = its complement (the
// 0xED71 convention the decoder uses: data = 1st half of each transitioning pair).
#define MAN_BIT(bit)                 \
    do {                             \
        if(bit) {                    \
            EMIT(true, te);          \
            EMIT(false, te);         \
        } else {                     \
            EMIT(false, te);         \
            EMIT(true, te);          \
        }                            \
    } while(0)

    EMIT(false, te * 50);

    // Raw sync preamble 0xED71 — NOT Manchester. Each preamble bit is ONE raw
    // half-symbol; the decoder hunts the raw 16-bit pattern.
    static const uint8_t preamble[2] = {0xED, 0x71};
    for(int i = 0; i < 2; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 69 data bits, MSB first across b[0..8] (b[8] contributes its top 5 bits).
    for(int i = 0; i < KIA_DATA_BITS; i++) {
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
    tpms_protocol_encoder_tpms_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderKia* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, tpms_protocol_kia_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_KIA_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_kia_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_kia_yield(void* context) {
    TPMSProtocolEncoderKia* instance = context;

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
