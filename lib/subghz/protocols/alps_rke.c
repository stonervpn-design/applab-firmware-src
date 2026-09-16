#include "alps_rke.h"

#include "protocols_common.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <string.h>

/*
 * Alps FWB1U545 — car RKE keyfob decoder
 *
 * Ported from the rtl_433 device `alps_fwb1u545.c`.
 *
 * Physical layer (from rtl_433 r_device):
 *   - Modulation : FSK_PULSE_MANCHESTER_ZEROBIT
 *   - short_width: 500us   (Manchester half-bit)  -> te_short = 500, te_long = 1000
 *   - reset_limit: 1500us  (end-of-frame gap)
 *   - Carrier    : 433.92 MHz SRD (typical for these fobs; not stated in rtl_433)
 *
 * Frame: 76 Manchester bits (9 full bytes + a trailing 4-bit nibble):
 *   byte[0]      = 0x55                    preamble / sync
 *   byte[1..4]   = ID (32 bit, big-endian) serial
 *   byte[5] >> 4 = button (4 bit)
 *   byte[5]      = byte[6]                 button byte is duplicated (sanity check)
 *   byte[7..8]   = sequence (16 bit rolling counter)
 *   byte[9] >> 4 = 4 unknown bits (unused)
 *
 * rtl_433 has no CRC; it validates with:
 *   byte[0] == 0x55 && byte[5] == byte[6] && id != 0 && id != 0xffffffff
 * We keep exactly that gate (plus rejecting an all-preamble 0x55.../0xAA... id)
 * to avoid the false-positives this codebase has fought with fuzzy car fobs.
 *
 * feed() re-implements the rtl_433 pulse-slicer as a LevelDuration state
 * machine: the 0x55 preamble is an alternating-bit run (a train of te_short
 * pulses), so we lock on a run of shorts, Manchester-decode the stream that
 * follows into a bit buffer, then slide a window to find the 0x55 sync byte and
 * validate the 76-bit frame. Polarity is recovered automatically by retrying
 * with an inverted bitstream (covers the ZEROBIT convention either way).
 */

#define TAG "AlpsRke"

#define ALPS_RKE_FRAME_BITS   76 /* full Manchester frame length */
#define ALPS_RKE_NEED_BITS    72 /* bits we actually parse: byte[0..8] */
#define ALPS_RKE_MIN_PREAMBLE 8 /* consecutive te_short pulses to lock preamble */
#define ALPS_RKE_MAX_BITS     96 /* decoded-bit ring capacity */
#define ALPS_RKE_RING_DROP    16 /* bits dropped from the front when ring fills */
#define ALPS_RKE_PREDICT_COUNT 3U /* next-code lines emitted in get_string */

#ifdef ENABLE_EMULATE_FEATURE
/* Emulate/TX build only. The frame is 9 cleartext bytes Manchester-coded at the
 * te_short half-bit; we prepend a run of te_short pulse pairs so the decoder's
 * preamble hunter (>= ALPS_RKE_MIN_PREAMBLE consecutive shorts) locks, then let
 * its window search recover the 0x55 sync inside the decoded stream. */
#define ALPS_RKE_PREAMBLE_PAIRS 12U /* 24 te_short pulses -> locks the >=8 gate */
#define ALPS_RKE_RESET_GAP_US   2000U /* end-of-frame gap (> reset_limit 1500) */
#define ALPS_RKE_UPLOAD_CAPACITY ((ALPS_RKE_PREAMBLE_PAIRS * 2U) + (9U * 16U) + 2U)
_Static_assert(
    ALPS_RKE_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "ALPS_RKE_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

static const SubGhzBlockConst subghz_protocol_alps_rke_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 150,
    .min_count_bit_for_found = ALPS_RKE_FRAME_BITS,
};

typedef enum {
    AlpsRkeDecoderStepReset = 0,
    AlpsRkeDecoderStepPreamble,
    AlpsRkeDecoderStepData,
} AlpsRkeDecoderStep;

struct SubGhzProtocolDecoderAlpsRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;
    uint16_t bit_count;
    ManchesterState manchester_state;
    uint8_t bits[ALPS_RKE_MAX_BITS];
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderAlpsRke {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};
#endif

// Cleartext next-code helper: the 16-bit sequence is a plain counter with no
// keyed/encrypted authenticator, so a future frame is the same 64-bit payload
// with only the low 16 bits replaced by (sequence + i). Lives outside the
// emulate guard so decode-only builds can still display predicted codes.
static uint64_t alps_rke_predict_data(uint64_t data, uint16_t seq_next) {
    return (data & 0xFFFFFFFFFFFF0000ULL) | (uint64_t)seq_next;
}


// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_alps_rke_decoder = {
    .alloc = subghz_protocol_decoder_alps_rke_alloc,
    .free = subghz_protocol_decoder_alps_rke_free,
    .feed = subghz_protocol_decoder_alps_rke_feed,
    .reset = subghz_protocol_decoder_alps_rke_reset,
    .get_hash_data = subghz_protocol_decoder_alps_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_alps_rke_serialize,
    .deserialize = subghz_protocol_decoder_alps_rke_deserialize,
    .get_string = subghz_protocol_decoder_alps_rke_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_alps_rke_encoder = {
    // Cleartext-rolling clone: byte-correct verbatim replay + counter re-encode.
    .alloc = subghz_protocol_encoder_alps_rke_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_alps_rke_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_alps_rke_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_alps_rke = {
    .name = ALPS_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_alps_rke_decoder,
    .encoder = &subghz_protocol_alps_rke_encoder,
};







// ============================================================================
// Encoder: build a byte-correct Manchester frame from the decoded payload.
// FSK_PULSE_MANCHESTER_ZEROBIT at te_short=500us half-bit, matching feed().
// ============================================================================
#ifdef ENABLE_EMULATE_FEATURE

void* subghz_protocol_encoder_alps_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAlpsRke* instance = calloc(1, sizeof(SubGhzProtocolEncoderAlpsRke));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_alps_rke;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, ALPS_RKE_UPLOAD_CAPACITY);

    return instance;
}

static void subghz_protocol_encoder_alps_rke_get_upload(SubGhzProtocolEncoderAlpsRke* instance) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = ALPS_RKE_UPLOAD_CAPACITY;
    const uint32_t te = subghz_protocol_alps_rke_const.te_short;

    /* Rebuild the 9 on-air bytes from generic.data = (id<<32)|(byte5<<24)|
     * (byte6<<16)|sequence. byte5/byte6 carry the captured button byte verbatim
     * (guaranteeing byte[5]==byte[6]); byte[9]'s trailing nibble is unused. */
    const uint64_t data = instance->generic.data;
    const uint32_t id = (uint32_t)(data >> 32);
    const uint8_t btn_byte = (uint8_t)((data >> 24) & 0xFF);
    const uint16_t seq = (uint16_t)(data & 0xFFFF);

    uint8_t bytes[9];
    bytes[0] = 0x55;
    bytes[1] = (uint8_t)(id >> 24);
    bytes[2] = (uint8_t)(id >> 16);
    bytes[3] = (uint8_t)(id >> 8);
    bytes[4] = (uint8_t)id;
    bytes[5] = btn_byte;
    bytes[6] = btn_byte;
    bytes[7] = (uint8_t)(seq >> 8);
    bytes[8] = (uint8_t)seq;

    size_t index = 0;
    /* Preamble: alternating te_short pulse pairs -> the run of shorts feed()
     * counts to lock its preamble gate before Manchester decoding begins. */
    index = pp_emit_short_pairs(up, index, cap, te, ALPS_RKE_PREAMBLE_PAIRS);
    /* 72 data bits, MSB-first Manchester at the same te the decoder uses. */
    for(size_t k = 0; k < 9; k++) {
        index = pp_emit_byte_manchester(up, index, cap, bytes[k], te);
    }
    /* End-of-frame reset gap so feed() finalizes the frame. */
    index = pp_emit(up, index, cap, false, ALPS_RKE_RESET_GAP_US);

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_alps_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderAlpsRke* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        if(subghz_block_generic_deserialize_check_count_bit(
               &instance->generic,
               flipper_format,
               subghz_protocol_alps_rke_const.min_count_bit_for_found) != SubGhzProtocolStatusOk) {
            break;
        }

        /* Derive editable fields from the captured payload, then let optional
         * Serial/Btn/Cnt overrides in the file steer them (reusable clone). */
        uint64_t data = instance->generic.data;
        uint32_t serial = (uint32_t)(data >> 32);
        const uint8_t captured_btn_byte = (uint8_t)((data >> 24) & 0xFF);
        uint32_t btn = (uint32_t)((captured_btn_byte >> 4) & 0x0F);
        uint32_t cnt = (uint32_t)(data & 0xFFFF);

        pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

        /* Preserve the captured low nibble of the button byte so byte[5]==byte[6]
         * stays true even when the high nibble (button) is overridden. */
        const uint8_t btn_byte = (uint8_t)(((btn & 0x0F) << 4) | (captured_btn_byte & 0x0F));
        const uint16_t seq = (uint16_t)(cnt & 0xFFFF);

        data = ((uint64_t)serial << 32) | ((uint64_t)btn_byte << 24) |
               ((uint64_t)btn_byte << 16) | (uint64_t)seq;
        instance->generic.data = data;
        instance->generic.data_count_bit = subghz_protocol_alps_rke_const.min_count_bit_for_found;
        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)(btn & 0x0F);
        instance->generic.cnt = seq;

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

        {
            uint8_t key_bytes[8];
            pp_u64_to_bytes_be(instance->generic.data, key_bytes);
            flipper_format_rewind(flipper_format);
            flipper_format_insert_or_update_hex(flipper_format, FF_KEY, key_bytes, sizeof(key_bytes));
        }
        pp_flipper_update_or_insert_u32(flipper_format, FF_SERIAL, serial);
        pp_flipper_update_or_insert_u32(flipper_format, FF_BTN, instance->generic.btn);
        pp_flipper_update_or_insert_u32(flipper_format, FF_CNT, seq);

        subghz_protocol_encoder_alps_rke_get_upload(instance);
        if(instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

#endif

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_alps_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAlpsRke* instance = malloc(sizeof(SubGhzProtocolDecoderAlpsRke));
    instance->base.protocol = &subghz_protocol_alps_rke;
    instance->generic.protocol_name = subghz_protocol_alps_rke.name;
    return instance;
}

void subghz_protocol_decoder_alps_rke_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_alps_rke_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;
    instance->decoder.parser_step = AlpsRkeDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->manchester_state = ManchesterStateMid1;
}

// ============================================================================
// Frame parsing: slide a window over the decoded bits, find the 0x55 sync
// byte and validate the rtl_433 field layout.
// ============================================================================

static bool subghz_protocol_alps_rke_try_decode(SubGhzProtocolDecoderAlpsRke* instance, bool invert) {
    const uint16_t count = instance->bit_count;
    if(count < ALPS_RKE_NEED_BITS) return false;

    int best_off = -1;
    uint8_t best_bytes[9] = {0};

    for(int off = 0; off + ALPS_RKE_NEED_BITS <= (int)count; off++) {
        uint8_t bytes[9];
        for(int k = 0; k < 9; k++) {
            uint8_t b = 0;
            for(int j = 0; j < 8; j++) {
                uint8_t bit = instance->bits[off + (k * 8) + j];
                if(invert) bit ^= 1u;
                b = (uint8_t)((b << 1) | (bit & 1u));
            }
            bytes[k] = b;
        }

        /* rtl_433 sanity: preamble byte and duplicated button byte */
        if(bytes[0] != 0x55) continue;
        if(bytes[5] != bytes[6]) continue;

        uint32_t id = ((uint32_t)bytes[1] << 24) | ((uint32_t)bytes[2] << 16) |
                      ((uint32_t)bytes[3] << 8) | (uint32_t)bytes[4];
        if(id == 0 || id == 0xFFFFFFFF) continue;
        /* Reject a window that sits entirely inside the alternating preamble
         * (which decodes to 0x55.../0xAA...) and would masquerade as a frame. */
        if(id == 0x55555555 || id == 0xAAAAAAAA) continue;

        /* Keep the last valid alignment: the real 0x55 sits at the tail of the
         * preamble run, immediately before the ID. */
        best_off = off;
        memcpy(best_bytes, bytes, sizeof(bytes));
    }

    if(best_off < 0) return false;

    uint32_t id = ((uint32_t)best_bytes[1] << 24) | ((uint32_t)best_bytes[2] << 16) |
                  ((uint32_t)best_bytes[3] << 8) | (uint32_t)best_bytes[4];
    uint8_t button = best_bytes[5] >> 4;
    uint16_t sequence = (uint16_t)(((uint16_t)best_bytes[7] << 8) | best_bytes[8]);

    /* Pack the unique payload (ID + button bytes + sequence) into 64 bits. */
    uint64_t data = ((uint64_t)id << 32) | ((uint64_t)best_bytes[5] << 24) |
                    ((uint64_t)best_bytes[6] << 16) | (uint64_t)sequence;

    instance->generic.data = data;
    instance->generic.data_count_bit = ALPS_RKE_FRAME_BITS;
    instance->generic.serial = id;
    instance->generic.btn = button;
    instance->generic.cnt = sequence;

    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = ALPS_RKE_FRAME_BITS;

    FURI_LOG_D(TAG, "Frame: Id:%08lX Btn:%X Cnt:%04X", (unsigned long)id, button, sequence);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    return true;
}

static void subghz_protocol_alps_rke_finalize(SubGhzProtocolDecoderAlpsRke* instance) {
    if(!subghz_protocol_alps_rke_try_decode(instance, false)) {
        subghz_protocol_alps_rke_try_decode(instance, true);
    }
    instance->decoder.parser_step = AlpsRkeDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_count = 0;
}

static void subghz_protocol_alps_rke_store_bit(SubGhzProtocolDecoderAlpsRke* instance, bool bit) {
    if(instance->bit_count >= ALPS_RKE_MAX_BITS) {
        /* Ring down: keep the most recent bits (the real frame is nearest the
         * end-of-frame gap), drop the oldest preamble bits. */
        memmove(
            instance->bits,
            instance->bits + ALPS_RKE_RING_DROP,
            ALPS_RKE_MAX_BITS - ALPS_RKE_RING_DROP);
        instance->bit_count = ALPS_RKE_MAX_BITS - ALPS_RKE_RING_DROP;
    }
    instance->bits[instance->bit_count++] = bit ? 1u : 0u;
}

static void subghz_protocol_alps_rke_manchester_feed(
    SubGhzProtocolDecoderAlpsRke* instance,
    bool level,
    uint32_t duration) {
    ManchesterEvent event =
        pp_manchester_event(duration, level, &subghz_protocol_alps_rke_const);
    if(event == ManchesterEventReset) {
        /* Bad timing / end-of-frame gap: try to decode what we have. */
        subghz_protocol_alps_rke_finalize(instance);
        return;
    }

    bool data_bit = false;
    if(manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
        subghz_protocol_alps_rke_store_bit(instance, data_bit);
    }
}

// ============================================================================
// Decoder: feed
// ============================================================================

void subghz_protocol_decoder_alps_rke_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;

    const SubGhzBlockConst* t = &subghz_protocol_alps_rke_const;
    bool is_short = DURATION_DIFF(duration, t->te_short) < t->te_delta;

    switch(instance->decoder.parser_step) {
    case AlpsRkeDecoderStepReset:
        if(is_short) {
            instance->preamble_count = 1;
            instance->decoder.parser_step = AlpsRkeDecoderStepPreamble;
        }
        break;

    case AlpsRkeDecoderStepPreamble:
        if(is_short) {
            instance->preamble_count++;
            if(instance->preamble_count >= ALPS_RKE_MIN_PREAMBLE) {
                /* Locked on the alternating-bit (0x55) preamble train. Start
                 * Manchester-decoding from here so the ID is captured in full;
                 * the 0x55 sync byte is recovered later by the window search. */
                instance->decoder.parser_step = AlpsRkeDecoderStepData;
                instance->bit_count = 0;
                instance->manchester_state = ManchesterStateMid1;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                subghz_protocol_alps_rke_manchester_feed(instance, level, duration);
            }
        } else {
            /* Preamble broken before lock -> noise. */
            instance->decoder.parser_step = AlpsRkeDecoderStepReset;
            instance->preamble_count = 0;
        }
        break;

    case AlpsRkeDecoderStepData:
        subghz_protocol_alps_rke_manchester_feed(instance, level, duration);
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / string
// ============================================================================

uint8_t subghz_protocol_decoder_alps_rke_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_alps_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_alps_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_alps_rke_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_alps_rke_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAlpsRke* instance = context;

    /* Re-derive fields from data so display is correct after load. */
    uint64_t data = instance->generic.data;
    uint32_t id = (uint32_t)(data >> 32);
    uint8_t button = (uint8_t)((data >> 28) & 0x0F);
    uint16_t sequence = (uint16_t)(data & 0xFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Id:%08lX Btn:%X\r\n"
        "Cnt:%04X\r\n"
        "Key:%016llX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)id,
        button,
        sequence,
        (unsigned long long)data);

    // Cleartext rolling counter with no keyed authenticator: re-pack the payload
    // with sequence+i to predict the next codes. These are byte-correct future
    // frames the real RX accepts (load/emulate/save-ready) -> reusable clone.
    for(uint16_t i = 1; i <= ALPS_RKE_PREDICT_COUNT; i++) {
        uint16_t seq_next = (uint16_t)(sequence + i);
        uint64_t next_data = alps_rke_predict_data(data, seq_next);
        furi_string_cat_printf(
            output, "\r\nNext+%u:%016llX", (unsigned int)i, (unsigned long long)next_data);
    }
}
