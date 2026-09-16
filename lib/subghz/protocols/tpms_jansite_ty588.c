#include "tpms_jansite_ty588.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJansiteTY588"

// Port of rtl_433/src/devices/tpms_jansite_ty588.c (GPL-2+).
//
// Jansite TY588-EU2 tyre pressure sensor. FSK 433.92 MHz, FSK_PULSE_PCM at
// ~51 us per raw half-symbol. Each frame is:
//   44 raw preamble bits  "99 aa 5a 6a 9a a"  (0x99AA5A6A9AA, NOT Manchester)
//   128 Manchester-coded bits -> 64 data bits (8 bytes B0..B7)
//   (2 trailing pad bits, ignored)
// rtl_433 searches the first 24 preamble bits (0x99aa5a), then
// bitbuffer_manchester_decode()s 64 bits starting at bitpos+44. Its Manchester
// convention emits the 2nd half of each transitioning pair, so the data bit is
// the 2nd half for the normal preamble (data_second=false path here).
//
// Wire format (after Manchester-decoding), all checks/scaling verbatim rtl_433:
//   B7 == B0                       (tail byte duplicates B0 -- the integrity MIC)
//   (B3 + B4) & 0xff == 0x30       (structural constant)
//   (B0 & 0x0f) == (B1 & 0x0f)     (structural: matching low nibbles)
//   temperature_C = ((B2 + B5) & 0xff) - 139
//   pressure_kPa  = (((B5 + B6) & 0xff) - 90) * 2.5   (reject if raw < 0)
// rtl_433 does not resolve which bits form the fixed sensor ID (it needs a
// second physical sensor to separate the ID from the payload) and instead
// reports B0..B6 as an opaque "code" string.
//
// ---- Our TPMSBlockGeneric ID mapping ----------------------------------------
// TPMSBlockGeneric carries a single uint32 id plus independent pressure/temp.
// The frame's information budget is exactly 44 bits: B7 duplicates B0 (-8),
// B4 = 0x30 - B3 (-8), B1's low nibble = B0's low nibble (-4)  =>  64-20 = 44.
// temperature and pressure spend 8 bits each, leaving 28 bits for the id. We
// pack the id into the bytes that are NOT pinned by temp/pressure:
//   B0        = id[27:20]
//   B1[7:4]   = id[19:16]      (B1[3:0] forced = B0[3:0])
//   B3        = id[15:8]       (B4 forced = 0x30 - B3)
//   B5        = id[7:0]        (also the shared temp/pressure offset byte)
// then B2 encodes temperature (via B5), B6 encodes pressure (via B5). Any id
// therefore round-trips its low 28 bits exactly; the top 4 bits are unstorable
// in this frame (the protocol has no wider clean ID). The validator id
// 0x0A5B6C7D fits in 28 bits, so it round-trips bit-for-bit.

#define JANSITE_PREAMBLE      0x99AA5A6A9AAULL
#define JANSITE_PREAMBLE_MASK 0x0FFFFFFFFFFFULL // 44-bit rolling hunt window
#define JANSITE_PREAMBLE_INV  ((~JANSITE_PREAMBLE) & JANSITE_PREAMBLE_MASK)
#define JANSITE_DATA_BITS     64

static const SubGhzBlockConst tpms_protocol_tpms_jansite_ty588_const = {
    .te_short = 51,
    .te_long = 51,
    .te_delta = 25,
    .min_count_bit_for_found = JANSITE_DATA_BITS,
};

typedef enum {
    JansiteDecoderStepReset = 0,
    JansiteDecoderStepFindPreamble,
    JansiteDecoderStepData,
} JansiteDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // FSK_PULSE_PCM: the frame opens with a fixed 44-bit raw preamble (one raw
    // half-symbol per bit, NOT Manchester), then the 64-bit payload is Manchester
    // coded (128 raw half-symbols). We hunt the preamble in the RAW half-symbol
    // stream, then pair the following half-symbols. Matches rtl_433 tpms_jansite:
    // preamble searched in raw bits, only the data Manchester-decoded.
    uint64_t raw_shift; // rolling 44-bit window of raw half-symbol bits for the hunt
    bool in_data; // true once the preamble matched, collecting Manchester data
    bool data_second; // false=normal preamble (data bit=2nd half, per rtl_433);
                      // true=inverted polarity (data bit=1st half)
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
} TPMSProtocolDecoderJansiteTY588;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderJansiteTY588;

const SubGhzProtocolDecoder tpms_protocol_tpms_jansite_ty588_decoder = {
    .alloc = tpms_protocol_decoder_tpms_jansite_ty588_alloc,
    .free = tpms_protocol_decoder_tpms_jansite_ty588_free,
    .feed = tpms_protocol_decoder_tpms_jansite_ty588_feed,
    .reset = tpms_protocol_decoder_tpms_jansite_ty588_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_jansite_ty588_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_jansite_ty588_serialize,
    .deserialize = tpms_protocol_decoder_tpms_jansite_ty588_deserialize,
    .get_string = tpms_protocol_decoder_tpms_jansite_ty588_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_jansite_ty588_encoder = {
    .alloc = tpms_protocol_encoder_tpms_jansite_ty588_alloc,
    .free = tpms_protocol_encoder_tpms_jansite_ty588_free,
    .deserialize = tpms_protocol_encoder_tpms_jansite_ty588_deserialize,
    .stop = tpms_protocol_encoder_tpms_jansite_ty588_stop,
    .yield = tpms_protocol_encoder_tpms_jansite_ty588_yield,
};

const SubGhzProtocol tpms_protocol_tpms_jansite_ty588 = {
    .name = TPMS_PROTOCOL_JANSITE_TY588_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_jansite_ty588_decoder,
    .encoder = &tpms_protocol_tpms_jansite_ty588_encoder,
};

void* tpms_protocol_decoder_tpms_jansite_ty588_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansiteTY588* instance =
        calloc(1, sizeof(TPMSProtocolDecoderJansiteTY588));
    instance->base.protocol = &tpms_protocol_tpms_jansite_ty588;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_jansite_ty588_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_jansite_ty588_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

static void jansite_get_bytes(TPMSProtocolDecoderJansiteTY588* instance, uint8_t b[8]) {
    // 64-bit frame: b[0] = MSB .. b[7] = LSB of decode_data.
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// The three rtl_433 integrity/structural checks. B7==B0 is the only real MIC.
static bool jansite_check_mic(TPMSProtocolDecoderJansiteTY588* instance) {
    uint8_t b[8];
    jansite_get_bytes(instance, b);
    if(b[7] != b[0]) return false;
    if(((b[3] + b[4]) & 0xff) != 0x30) return false;
    if((b[0] & 0x0f) != (b[1] & 0x0f)) return false;
    return true;
}

static bool jansite_analyze(TPMSProtocolDecoderJansiteTY588* instance) {
    uint8_t b[8];
    jansite_get_bytes(instance, b);

    int raw_temp = (b[2] + b[5]) & 0xff;
    int raw_pressure = (b[5] + b[6]) & 0xff;
    int temperature = raw_temp - 139;
    int pressure_raw = raw_pressure - 90;

    // rtl_433 plausibility guard.
    if(pressure_raw < 0 || temperature < -40 || temperature > 120) {
        return false;
    }

    // kPa = pressure_raw * 2.5; store BAR (kPa/100) in the generic block.
    instance->generic.pressure = (float)pressure_raw * 2.5f * 0.01f;
    instance->generic.temperature = (float)temperature;
    // 28-bit id recovered from the non-pinned bytes (see header comment).
    instance->generic.id = ((uint32_t)b[0] << 20) | ((uint32_t)(b[1] >> 4) << 16) |
                           ((uint32_t)b[3] << 8) | (uint32_t)b[5];
    instance->generic.battery_low = TPMS_NO_BATT;
    return true;
}

// Consume one raw half-symbol bit: hunt the 44-bit raw preamble (0x99AA5A6A9AA,
// or the inverted-polarity value), then Manchester-decode the 64-bit payload.
// rtl_433's bitbuffer_manchester_decode emits the 2nd half of each pair, so the
// data bit is the 2nd half for the normal preamble and the 1st half when the FSK
// demod polarity is inverted.
static void jansite_emit_raw_bit(TPMSProtocolDecoderJansiteTY588* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = ((instance->raw_shift << 1) | bit) & JANSITE_PREAMBLE_MASK;
        if(instance->raw_shift == JANSITE_PREAMBLE ||
           instance->raw_shift == JANSITE_PREAMBLE_INV) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == JANSITE_PREAMBLE_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 2nd half (normal preamble)
    // or the 1st half (inverted polarity). No transition = end/garbage -> resync.
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
    uint8_t data_bit = instance->data_second ? instance->first_half : bit;
    subghz_protocol_blocks_add_bit(&instance->decoder, data_bit);
    if(instance->decoder.decode_count_bit == JANSITE_DATA_BITS) {
        if(jansite_check_mic(instance) && jansite_analyze(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_tpms_jansite_ty588_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;

    const uint32_t te = tpms_protocol_tpms_jansite_ty588_const.te_short; // 51us half-symbol
    // Both the raw preamble and the Manchester payload never exceed a run of two
    // identical half-symbols, so a gap (or any pulse longer than two half-symbols)
    // ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_jansite_ty588_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        jansite_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_jansite_ty588_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_jansite_ty588_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_jansite_ty588_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_jansite_ty588_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_jansite_ty588_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY588* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C  %.0f kPa (%2.1f bar)",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)(instance->generic.pressure * 100.0f),
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer
// -----------------------------------------------------------------------------
//
// Invert jansite_analyze(): synthesize the 8 wire bytes so all three structural
// checks hold and the id/temperature/pressure decode back exactly (id truncated
// to 28 bits -- the frame's information budget; see header). B5 doubles as the
// low id byte and the shared temp/pressure offset, so B2 and B6 are derived from
// it. Stores the bytes MSB-first in g->data and sets g->data_count_bit = 64.
void tpms_protocol_tpms_jansite_ty588_pack(TPMSBlockGeneric* g) {
    furi_assert(g);

    // pressure: bar -> kPa (*100) -> raw (/2.5). Clamp raw to the range that adds
    // to B5 without wrapping the byte (decoder rejects raw < 0).
    int p_raw = (int)(g->pressure * 100.0f / 2.5f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 165) p_raw = 165;

    int temperature = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f));
    if(temperature < -40) temperature = -40;
    if(temperature > 116) temperature = 116; // (temp+139) must stay in a byte
    int raw_temp = (temperature + 139) & 0xff;

    uint32_t id = g->id;
    uint8_t b[8];
    b[0] = (uint8_t)((id >> 20) & 0xff); // id[27:20]
    b[1] = (uint8_t)((((id >> 16) & 0x0f) << 4) | (b[0] & 0x0f)); // id[19:16] : B0 low nibble
    b[3] = (uint8_t)((id >> 8) & 0xff); // id[15:8]
    b[4] = (uint8_t)((0x30 - b[3]) & 0xff); // (B3+B4)&0xff == 0x30
    b[5] = (uint8_t)(id & 0xff); // id[7:0]  (shared offset byte)
    b[2] = (uint8_t)((raw_temp - b[5]) & 0xff); // (B2+B5)&0xff == temp+139
    b[6] = (uint8_t)(((p_raw + 90) - b[5]) & 0xff); // (B5+B6)&0xff == p_raw+90
    b[7] = b[0]; // tail == B0 (the MIC)

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = JANSITE_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK. Frame = inter-frame gap + RAW 44-bit preamble 0x99AA5A6A9AA (one raw
// half-symbol per bit, MSB first -- NOT Manchester) + 64 Manchester-coded data
// bits. The decoder takes the 2nd half of each transitioning pair as the data
// bit (data_second=false / normal-preamble path), so:
//   bit 1 -> 1st half LOW,  2nd half HIGH   (LOW-then-HIGH)
//   bit 0 -> 1st half HIGH, 2nd half LOW    (HIGH-then-LOW)
// build_frame re-derives every dependent byte so an edited/0-PSI frame always
// satisfies the decoder's checks.
//
// Worst-case entries: 1 gap + 44 raw preamble + 2*64 data + 1 = 174.
#define TPMS_JANSITE_TY588_ENCODER_UPLOAD_SIZE 220

void* tpms_protocol_encoder_tpms_jansite_ty588_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderJansiteTY588* instance =
        malloc(sizeof(TPMSProtocolEncoderJansiteTY588));
    instance->base.protocol = &tpms_protocol_tpms_jansite_ty588;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_JANSITE_TY588_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_jansite_ty588_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderJansiteTY588* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_jansite_ty588_stop(void* context) {
    TPMSProtocolEncoderJansiteTY588* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 8-byte frame from generic.data and RE-DERIVE the dependent
// bytes (B4 from B3, B7 from B0) so an edited/0-PSI frame is always valid.
static void tpms_protocol_encoder_tpms_jansite_ty588_build_frame(
    TPMSBlockGeneric* generic,
    uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[4] = (uint8_t)((0x30 - b[3]) & 0xff); // (B3+B4)&0xff == 0x30
    b[1] = (uint8_t)((b[1] & 0xf0) | (b[0] & 0x0f)); // B1 low nibble == B0 low nibble
    b[7] = b[0]; // tail == B0 (the MIC)
}

static bool tpms_protocol_encoder_tpms_jansite_ty588_get_upload(
    TPMSProtocolEncoderJansiteTY588* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_jansite_ty588_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_tpms_jansite_ty588_build_frame(&instance->generic, frame);

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

// Manchester data bit (Jansite/rtl_433 convention: the decoder takes the 2nd half
// of each transitioning pair): 1st half = complement, 2nd half = the data bit.
#define MAN_BIT(bit)                 \
    do {                             \
        if(bit) {                    \
            EMIT(false, te);         \
            EMIT(true, te);          \
        } else {                     \
            EMIT(true, te);          \
            EMIT(false, te);         \
        }                            \
    } while(0)

    EMIT(false, te * 50);

    // Raw 44-bit preamble 0x99AA5A6A9AA -- NOT Manchester. The decoder hunts this
    // pattern one raw half-symbol at a time in a 44-bit window, so each preamble
    // bit is ONE raw half-symbol (MSB first). Once the 44th bit matches, the
    // decoder starts pairing the data half-symbols that follow.
    for(int bpos = 43; bpos >= 0; bpos--) {
        EMIT(((JANSITE_PREAMBLE >> bpos) & 0x1ULL) ? true : false, te);
    }

    // 64 data bits, MSB first (frame[0]..frame[7])
    for(int i = 0; i < JANSITE_DATA_BITS; i++) {
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

SubGhzProtocolStatus tpms_protocol_encoder_tpms_jansite_ty588_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderJansiteTY588* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_jansite_ty588_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_JANSITE_TY588_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_jansite_ty588_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_jansite_ty588_yield(void* context) {
    TPMSProtocolEncoderJansiteTY588* instance = context;

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
