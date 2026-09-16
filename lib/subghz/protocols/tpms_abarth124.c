#include "tpms_abarth124.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSAbarth124"

// Port of rtl_433/src/devices/tpms_abarth124.c
//   (VDO Type TG1C, TTigges / based on Jansite Solar TPMS; GPL-2+).
//
// This ports the VDO TG1C variant only (Abarth 124 Spider, also some Fiat 124
// Spider and Mazda MX-5 ND). The Shenzhen EGQ Q85 variant (12-byte / 96-bit)
// carried in the same rtl_433 file is a different device and is not ported here.
//
// RF: FSK 433.92 MHz, standard Manchester, 9-byte (72-bit) payload.
// On air preamble (raw, before Manchester decode) is 0x55 0x55 0x56
//   (rtl_433 inverts and searches 0xAA 0xAA 0xA9). The trailing 16-bit sync
//   word (raw 0x5556) is identical to the Renault TPMS preamble tail, so the
//   same Manchester + preamble machinery decodes both; only the byte layout
//   and integrity check differ.
//
// Wire format (after Manchester decode, 9 bytes b[0..8]):
//   II II II II ?? PP TT SS CC
//   b[0..3] = 32-bit ID, big-endian
//   b[4]    = flags / status nibble
//   b[5]    = pressure raw     -> kPa = raw * 1.38
//   b[6]    = temperature raw  -> C   = raw - 50
//   b[7]    = status
//   b[8]    = checksum: XOR over b[0..8] == 0  (i.e. b[8] = XOR(b[0..7]))
//
// The 8-bit XOR checksum is weak (~1/256 false-accept) and this decoder shares
// its preamble/modulation with other 433 MHz TPMS (e.g. Renault). rtl_433 adds
// a temperature plausibility bound (TG1C working range -50..125 C) as a second
// independent filter; we enforce the same gate before the callback.

#define ABARTH124_PREAMBLE 0x5556U
#define ABARTH124_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_abarth124_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = ABARTH124_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Abarth124 is FSK where the frame is RAW half-symbols: a fixed sync preamble
    // in the raw bits, then the 72-bit payload is Manchester-encoded (matches
    // rtl_433 tpms_abarth124: search 0x5556/0xAAA9 in raw bits, then
    // manchester-decode only the data). The old port ran manchester over
    // EVERYTHING and hunted the preamble in the manchester-decoded stream, which
    // never syncs.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderAbarth124;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderAbarth124;

void* tpms_protocol_encoder_abarth124_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_abarth124_free(void* context);
void tpms_protocol_encoder_abarth124_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_abarth124_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_abarth124_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_abarth124_decoder = {
    .alloc = tpms_protocol_decoder_abarth124_alloc,
    .free = tpms_protocol_decoder_abarth124_free,
    .feed = tpms_protocol_decoder_abarth124_feed,
    .reset = tpms_protocol_decoder_abarth124_reset,
    .get_hash_data = tpms_protocol_decoder_abarth124_get_hash_data,
    .serialize = tpms_protocol_decoder_abarth124_serialize,
    .deserialize = tpms_protocol_decoder_abarth124_deserialize,
    .get_string = tpms_protocol_decoder_abarth124_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_abarth124_encoder = {
    .alloc = tpms_protocol_encoder_abarth124_alloc,
    .free = tpms_protocol_encoder_abarth124_free,
    .deserialize = tpms_protocol_encoder_abarth124_deserialize,
    .stop = tpms_protocol_encoder_abarth124_stop,
    .yield = tpms_protocol_encoder_abarth124_yield,
};

const SubGhzProtocol tpms_protocol_abarth124 = {
    .name = TPMS_PROTOCOL_ABARTH124_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_abarth124_decoder,
    .encoder = &tpms_protocol_abarth124_encoder,
};

void* tpms_protocol_decoder_abarth124_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderAbarth124* instance = calloc(1, sizeof(TPMSProtocolDecoderAbarth124));
    instance->base.protocol = &tpms_protocol_abarth124;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_abarth124_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_abarth124_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

static void abarth124_get_bytes(TPMSProtocolDecoderAbarth124* instance, uint8_t b[9]) {
    // 72 bits: add_to_128_bit keeps the upper 8 bits in head_64_bit (= b[0]) and
    // the low 64 bits in decode_data (= b[1..8]).
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool abarth124_check(TPMSProtocolDecoderAbarth124* instance) {
    uint8_t b[9];
    abarth124_get_bytes(instance, b);
    // Checksum8 XOR over bytes 0..8 must be zero.
    if(subghz_protocol_blocks_xor_bytes(b, 9) != 0) return false;
    // Temperature plausibility (TG1C working range -50..125 C): temp_raw in [0,175].
    int temp_c = (int)b[6] - 50;
    if(temp_c < -50 || temp_c > 125) return false;
    return true;
}

static void abarth124_analyze(TPMSProtocolDecoderAbarth124* instance) {
    uint8_t b[9];
    abarth124_get_bytes(instance, b);
    instance->generic.id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                           ((uint32_t)b[2] << 8) | b[3];
    instance->generic.pressure = (float)b[5] * 1.38f; // kPa
    instance->generic.temperature = (float)((int)b[6] - 50);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 72-bit payload. Matches
// rtl_433 tpms_abarth124 (preamble searched in raw bits; only the data is
// Manchester). The 72-bit frame keeps its upper 8 bits in head_64_bit via
// add_to_128_bit, exactly as the original manchester path did.
static void abarth124_emit_raw_bit(TPMSProtocolDecoderAbarth124* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == 0x5556 || instance->raw_shift == 0xAAA9) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == 0xAAA9);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (0x5556) or 2nd
    // half (0xAAA9). No transition = end/garbage -> resync on the preamble.
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
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == ABARTH124_DATA_BITS) {
        if(abarth124_check(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            abarth124_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_abarth124_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;

    const uint32_t te = tpms_protocol_abarth124_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_abarth124_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        abarth124_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_abarth124_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_abarth124_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_abarth124_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_abarth124_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_abarth124_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C  %3.0f kPa",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer (invert analyze) + Encoder
// -----------------------------------------------------------------------------
//
// FSK standard Manchester (G.E.Thomas / non-inverting, same family as Renault &
// Citroen): the 72-bit payload is Manchester-encoded, preceded by a RAW sync
// preamble that ends in 0x5556 (the decoder hunts 0x5556 / inverted 0xAAA9 in the
// raw half-symbol stream, then Manchester-decodes only the data).
//   Manchester bit 1 -> 1st half HIGH, 2nd half LOW  (data = 1st half, 0x5556 conv)
//   Manchester bit 0 -> 1st half LOW,  2nd half HIGH
// Frame: gap (reset) + 0x55 0x55 0x56 raw preamble + 72 Manchester data bits.
//
// Wire frame b[0..8] (see decoder): b[0..3]=ID BE, b[4]=flags, b[5]=pressure raw,
// b[6]=temp raw, b[7]=status, b[8]=XOR(b[0..7]). generic.data holds the low 64
// bits (b[1..8]); b[0] (ID top byte) is restored from generic.id>>24. The XOR
// checksum b[8] is recomputed on build so a modified / 0-PSI frame stays valid.
//
// Worst-case entries: 1 gap + 24 preamble + 2*72 data + 1 = 170.
#define TPMS_ABARTH124_ENCODER_UPLOAD_SIZE 200

void tpms_protocol_abarth124_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Decoder: kPa = raw * 1.38. The harness supplies pressure in bar, so the
    // native kPa value is bar*100 -> raw = bar*100 / 1.38.
    int p_raw = (int)(g->pressure * 100.0f / 1.38f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // Decoder: C = raw - 50 -> raw = C + 50.
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t b[9];
    b[0] = (uint8_t)((g->id >> 24) & 0xFF); // ID big-endian
    b[1] = (uint8_t)((g->id >> 16) & 0xFF);
    b[2] = (uint8_t)((g->id >> 8) & 0xFF);
    b[3] = (uint8_t)(g->id & 0xFF);
    b[4] = 0; // flags / status nibble
    b[5] = (uint8_t)p_raw; // pressure raw
    b[6] = (uint8_t)t; // temperature raw
    b[7] = 0; // status
    b[8] = subghz_protocol_blocks_xor_bytes(b, 8); // XOR(b[0..7]) => XOR(b[0..8])==0

    // generic.data holds the low 64 bits b[1..8]; b[0] rides in generic.id top byte.
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = ABARTH124_DATA_BITS;
}

void* tpms_protocol_encoder_abarth124_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderAbarth124* instance = malloc(sizeof(TPMSProtocolEncoderAbarth124));
    instance->base.protocol = &tpms_protocol_abarth124;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_ABARTH124_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_abarth124_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderAbarth124* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_abarth124_stop(void* context) {
    TPMSProtocolEncoderAbarth124* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 9-byte frame. b[1..8] come from generic.data; b[0] (ID top
// byte) is restored from generic.id; b[8] is recomputed as XOR(b[0..7]) so any
// edited / 0-PSI frame satisfies the decoder's checksum.
static void
    tpms_protocol_encoder_abarth124_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[8] = subghz_protocol_blocks_xor_bytes(b, 8); // XOR(b[0..7])
}

static bool tpms_protocol_encoder_abarth124_get_upload(TPMSProtocolEncoderAbarth124* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_abarth124_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_abarth124_build_frame(&instance->generic, frame);

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
// 0x5556 convention the decoder uses: data = 1st half of each transitioning pair).
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

    // Raw sync preamble — NOT Manchester. The decoder hunts the raw 0x5556 (or
    // inverted 0xAAA9) pattern in the half-symbol stream, so each preamble bit is
    // ONE raw half-symbol; only the data below is Manchester-encoded.
    static const uint8_t preamble[3] = {0x55, 0x55, 0x56};
    for(int i = 0; i < 3; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 72 data bits, MSB first (frame[0]..frame[8])
    for(int i = 0; i < ABARTH124_DATA_BITS; i++) {
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
    tpms_protocol_encoder_abarth124_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderAbarth124* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_abarth124_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_ABARTH124_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_abarth124_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_abarth124_yield(void* context) {
    TPMSProtocolEncoderAbarth124* instance = context;

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
