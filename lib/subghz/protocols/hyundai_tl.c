#include "hyundai_tl.h"
#include "protocols_common.h"

// --- pulse-distance timings (us), measured + round-trip verified ---
#define TL_MARK 73 // fixed high pulse
#define TL_GAP0 100 // low gap -> bit 0
#define TL_GAP1 150 // low gap -> bit 1
#define TL_GAP_MID 125 // 0/1 threshold
#define TL_GAP_SEP 330 // block/frame separator
#define TL_GAP_END 900 // terminator gap (TX) / end-of-frame (RX)
#define TL_MARK_DELTA 45
#define TL_END_US 700 // RX: gap longer than this = end of transmission

#define TL_PREAMBLE_MIN 32 // RX: consecutive bit-pairs before we accept a preamble
#define TL_PREAMBLE_TX 256 // TX: preamble bit-pairs to emit (AGC/clock training)
#define TL_PAYLOAD_MIN 40 // minimum payload bits to publish
#define TL_PAYLOAD_MAX 192 // buffer cap (bits)
#define TL_BUF_BYTES ((TL_PAYLOAD_MAX + 7) / 8)

#define TL_FF_BITS "TL_Bits" // custom flipper_format field holding the full payload

typedef enum {
    TlStepReset = 0, // hunting for a mark
    TlStepPreamble, // counting preamble bit-pairs
    TlStepMark, // saw a mark, waiting for its gap (in payload)
    TlStepPayload, // collecting payload, expecting a mark
} TlStep;

struct SubGhzProtocolDecoderHyundaiTl {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    TlStep step;
    uint16_t preamble_pairs;
    uint16_t bit_count;
    uint8_t buf[TL_BUF_BYTES];

    // snapshot of the last published payload, preserved for serialize()
    uint8_t pub_buf[TL_BUF_BYTES];
    uint16_t pub_bits;
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderHyundaiTl {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t buf[TL_BUF_BYTES];
    uint16_t bit_count;
};
#endif

// ---------------- decoder ----------------

static void tl_clear_collect(SubGhzProtocolDecoderHyundaiTl* i) {
    i->bit_count = 0;
    memset(i->buf, 0, sizeof(i->buf));
}

// re-arm after a publish WITHOUT wiping generic/pub_buf (framework holds those)
static void tl_rearm(SubGhzProtocolDecoderHyundaiTl* i) {
    i->step = TlStepReset;
    i->preamble_pairs = 0;
    tl_clear_collect(i);
}

static void tl_push_bit(SubGhzProtocolDecoderHyundaiTl* i, bool bit) {
    if(i->bit_count >= TL_PAYLOAD_MAX) return;
    if(bit) i->buf[i->bit_count >> 3] |= (uint8_t)(0x80U >> (i->bit_count & 7));
    i->bit_count++;
}

static bool tl_is_mark(uint32_t d) {
    return DURATION_DIFF(d, TL_MARK) < TL_MARK_DELTA;
}

static void tl_publish(SubGhzProtocolDecoderHyundaiTl* instance) {
    if(instance->bit_count < TL_PAYLOAD_MIN) return;

    // Pack the first 64 payload bits into the generic block for hash/dedup/display.
    uint64_t d = 0;
    uint16_t n = instance->bit_count < 64 ? instance->bit_count : 64;
    for(uint16_t b = 0; b < n; b++) {
        bool bit = (instance->buf[b >> 3] >> (7 - (b & 7))) & 1U;
        d = (d << 1) | (bit ? 1U : 0U);
    }
    instance->generic.data = d;
    instance->generic.data_count_bit = instance->bit_count;
    instance->generic.serial = (uint32_t)(d >> 32);
    instance->generic.btn = 0;
    instance->generic.cnt = 0;

    // snapshot the full payload so serialize() can write it after re-arm
    memcpy(instance->pub_buf, instance->buf, sizeof(instance->pub_buf));
    instance->pub_bits = instance->bit_count;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void* subghz_protocol_decoder_hyundai_tl_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHyundaiTl* instance = calloc(1, sizeof(SubGhzProtocolDecoderHyundaiTl));
    furi_check(instance);
    instance->base.protocol = &hyundai_tl_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_hyundai_tl_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_hyundai_tl_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;
    instance->step = TlStepReset;
    instance->preamble_pairs = 0;
    tl_clear_collect(instance);
    instance->generic.data = 0;
    instance->generic.data_count_bit = 0;
}

void subghz_protocol_decoder_hyundai_tl_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;

    switch(instance->step) {
    case TlStepReset:
        if(level && tl_is_mark(duration)) {
            instance->step = TlStepPreamble;
            instance->preamble_pairs = 0;
        }
        break;

    case TlStepPreamble:
        if(!level) {
            if(duration < TL_GAP_SEP - 60) {
                instance->preamble_pairs++;
            } else if(
                duration >= TL_GAP_SEP - 60 && duration < TL_END_US &&
                instance->preamble_pairs >= TL_PREAMBLE_MIN) {
                instance->step = TlStepPayload;
                tl_clear_collect(instance);
            } else {
                instance->step = TlStepReset;
                instance->preamble_pairs = 0;
            }
        } else {
            if(!tl_is_mark(duration)) {
                instance->step = TlStepReset;
                instance->preamble_pairs = 0;
            }
        }
        break;

    case TlStepPayload:
        if(level) {
            if(tl_is_mark(duration)) {
                instance->step = TlStepMark;
            } else {
                tl_publish(instance);
                tl_rearm(instance);
            }
        } else {
            tl_publish(instance);
            tl_rearm(instance);
        }
        break;

    case TlStepMark:
        if(!level) {
            if(duration >= TL_END_US) {
                tl_publish(instance);
                tl_rearm(instance);
            } else if(duration >= TL_GAP_SEP - 60) {
                instance->step = TlStepPayload; // block separator -> keep collecting
            } else {
                tl_push_bit(instance, duration >= TL_GAP_MID);
                instance->step = TlStepPayload;
            }
        } else {
            instance->step = TlStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_hyundai_tl_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, 8);
}

SubGhzProtocolStatus subghz_protocol_decoder_hyundai_tl_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        // persist the FULL payload (>64 bits) so the encoder can re-transmit it
        uint16_t nbytes = (uint16_t)((instance->pub_bits + 7) / 8);
        if(nbytes == 0) nbytes = 1;
        if(!flipper_format_write_hex(flipper_format, TL_FF_BITS, instance->pub_buf, nbytes)) {
            return SubGhzProtocolStatusError;
        }
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_hyundai_tl_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, TL_PAYLOAD_MIN);
}

void subghz_protocol_decoder_hyundai_tl_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiTl* instance = context;

    furi_string_cat_printf(
        output,
        "%s %ubit (rolling)\r\n"
        "Key:%08lX%08lX\r\n"
        "433.92 FSK\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
}

// ---------------- encoder ----------------
#ifdef ENABLE_EMULATE_FEATURE
static bool tl_build_upload(SubGhzProtocolEncoderHyundaiTl* instance) {
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = PP_SHARED_UPLOAD_CAPACITY;
    size_t idx = 0;

    // preamble: alternating short/long gaps (clock/AGC training)
    for(uint16_t k = 0; k < TL_PREAMBLE_TX; k++) {
        idx = pp_emit(up, idx, cap, true, TL_MARK);
        idx = pp_emit(up, idx, cap, false, (k & 1) ? TL_GAP1 : TL_GAP0);
    }
    // separator marks the preamble->payload boundary (decoder relies on this)
    idx = pp_emit(up, idx, cap, true, TL_MARK);
    idx = pp_emit(up, idx, cap, false, TL_GAP_SEP);
    // payload bits, MSB-first (matches decoder packing)
    for(uint16_t b = 0; b < instance->bit_count; b++) {
        bool bit = (instance->buf[b >> 3] >> (7 - (b & 7))) & 1U;
        idx = pp_emit(up, idx, cap, true, TL_MARK);
        idx = pp_emit(up, idx, cap, false, bit ? TL_GAP1 : TL_GAP0);
    }
    // terminator: final mark + long gap so the receiver publishes the frame
    idx = pp_emit(up, idx, cap, true, TL_MARK);
    idx = pp_emit(up, idx, cap, false, TL_GAP_END);

    if(idx > cap) return false;
    instance->encoder.front = 0;
    instance->encoder.size_upload = idx;
    return true;
}

void* subghz_protocol_encoder_hyundai_tl_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHyundaiTl* instance = calloc(1, sizeof(SubGhzProtocolEncoderHyundaiTl));
    furi_check(instance);
    instance->base.protocol = &hyundai_tl_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    pp_encoder_buffer_ensure(instance, PP_SHARED_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_hyundai_tl_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderHyundaiTl* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus st = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, TL_PAYLOAD_MIN);
    if(st != SubGhzProtocolStatusOk) return st;

    instance->bit_count = instance->generic.data_count_bit;
    if(instance->bit_count > TL_PAYLOAD_MAX) instance->bit_count = TL_PAYLOAD_MAX;
    memset(instance->buf, 0, sizeof(instance->buf));

    uint16_t nbytes = (uint16_t)((instance->bit_count + 7) / 8);
    flipper_format_rewind(flipper_format);
    if(!flipper_format_read_hex(flipper_format, TL_FF_BITS, instance->buf, nbytes)) {
        // Only files saved by this decoder carry the full payload; RAW-derived
        // or legacy files can't be transmitted as a decoded Hyundai TL frame.
        return SubGhzProtocolStatusErrorParserKey;
    }

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 3);
    if(!tl_build_upload(instance)) return SubGhzProtocolStatusError;
    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_hyundai_tl_decoder = {
    .alloc = subghz_protocol_decoder_hyundai_tl_alloc,
    .free = subghz_protocol_decoder_hyundai_tl_free,
    .feed = subghz_protocol_decoder_hyundai_tl_feed,
    .reset = subghz_protocol_decoder_hyundai_tl_reset,
    .get_hash_data = subghz_protocol_decoder_hyundai_tl_get_hash_data,
    .serialize = subghz_protocol_decoder_hyundai_tl_serialize,
    .deserialize = subghz_protocol_decoder_hyundai_tl_deserialize,
    .get_string = subghz_protocol_decoder_hyundai_tl_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_hyundai_tl_encoder = {
    .alloc = subghz_protocol_encoder_hyundai_tl_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_hyundai_tl_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_hyundai_tl_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol hyundai_tl_protocol = {
    .name = HYUNDAI_TL_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_hyundai_tl_decoder,
    .encoder = &subghz_protocol_hyundai_tl_encoder,
};
