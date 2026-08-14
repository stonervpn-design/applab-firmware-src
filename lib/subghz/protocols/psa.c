#include "psa.h"
#include "psa_crypto.h"
#include "protocols_common.h"

#define TAG "PSAProtocol"

// --- PSA RX log gate (framework copy) --------------------------------------
// The framework build keeps FURI_LOG enabled globally (lib/subghz/defines.h
// deliberately omits ProtoPirate's REMOVE_LOGS so the rest of the Sub-GHz app
// keeps logging). But PSA's per-frame decode/decrypt tracing is chatty on the RX
// hot path in the stock Read app and costs time on every repeat of every capture.
// Silence it for THIS translation unit only, mirroring ProtoPirate's REMOVE_LOGS
// convention (defining REMOVE_LOGS here also activates the log-only-variable
// #ifndef REMOVE_LOGS guards further down, so no vars go unused). The proto_pirate
// copy already builds with this exact mechanism. Build -DPSA_VERBOSE_LOG to restore.
#ifndef PSA_VERBOSE_LOG
#ifndef REMOVE_LOGS
#define REMOVE_LOGS
#endif
#undef FURI_LOG_E
#undef FURI_LOG_W
#undef FURI_LOG_I
#undef FURI_LOG_D
#undef FURI_LOG_T
#define FURI_LOG_E(tag, format, ...)
#define FURI_LOG_W(tag, format, ...)
#define FURI_LOG_I(tag, format, ...)
#define FURI_LOG_D(tag, format, ...)
#define FURI_LOG_T(tag, format, ...)
#endif // PSA_VERBOSE_LOG

static const SubGhzBlockConst subghz_protocol_psa_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 128,
};

#define PSA_TE_SHORT_125        0x7d
#define PSA_TE_LONG_250         0xfa
#define PSA_TE_END_1000         1000
#define PSA_TE_END_500          500
#define PSA_TOLERANCE_99        99
#define PSA_TOLERANCE_100       100
#define PSA_TOLERANCE_49        0x31
#define PSA_TOLERANCE_50        0x32
#define PSA_PATTERN_THRESHOLD_1 0x46
#define PSA_PATTERN_THRESHOLD_2 0x45
#define PSA_MAX_BITS            0x79
#define PSA_KEY1_BITS           0x40
#define PSA_KEY2_BITS           0x50
#define PSA_BUFFER_SIZE         48
#define PSA_TE_LONG_300         0x12c
#define PSA_UPLOAD_CAPACITY     325U
_Static_assert(
    PSA_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "PSA_UPLOAD_CAPACITY exceeds shared upload slab");

typedef enum {
    PSADecoderState0 = 0,
    PSADecoderState1 = 1,
    PSADecoderState2 = 2,
    PSADecoderState3 = 3,
    PSADecoderState4 = 4,
} PSADecoderState;

struct SubGhzProtocolDecoderPSA {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;

    uint32_t state;
    uint32_t prev_duration;

    uint32_t decode_data_low;
    uint32_t decode_data_high;
    uint8_t decode_count_bit;

    uint32_t seed;
    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t key2_high;

    uint32_t status_flag;
    uint16_t decrypted;
    uint8_t mode_serialize;

    uint16_t pattern_counter;
    ManchesterState manchester_state;

    uint8_t decrypted_button;
    uint32_t decrypted_serial;
    uint32_t decrypted_counter;
    uint16_t decrypted_crc;
    uint32_t decrypted_seed;
    uint8_t decrypted_type;
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderPSA {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t key1_low;
    uint32_t key1_high;
    uint16_t validation_field;
    uint32_t key2_low;
    uint32_t counter;
    uint8_t button;
    uint8_t type;
    uint8_t seed;
    uint8_t mode;
    uint32_t serial;
    uint16_t crc;
    bool is_running;
};
#endif
const SubGhzProtocolDecoder subghz_protocol_psa_decoder = {
    .alloc = subghz_protocol_decoder_psa_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_psa_feed,
    .reset = subghz_protocol_decoder_psa_reset,
    .get_hash_data = subghz_protocol_decoder_psa_get_hash_data,
    .serialize = subghz_protocol_decoder_psa_serialize,
    .deserialize = subghz_protocol_decoder_psa_deserialize,
    .get_string = subghz_protocol_decoder_psa_get_string,
};
#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_psa_encoder = {
    .alloc = subghz_protocol_encoder_psa_alloc,
    .free = subghz_protocol_encoder_psa_free,
    .deserialize = subghz_protocol_encoder_psa_deserialize,
    .stop = subghz_protocol_encoder_psa_stop,
    .yield = subghz_protocol_encoder_psa_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_psa_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif
const SubGhzProtocol psa_protocol = {
    .name = PSA_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_psa_decoder,
    .encoder = &subghz_protocol_psa_encoder,
};

static void psa_calculate_checksum(uint8_t* buffer);
static void psa_second_stage_xor_decrypt(uint8_t* buffer);

#ifdef ENABLE_EMULATE_FEATURE
// Manchester event -> physical (level, duration) pulse, mirroring the decoder's
// State2/State4 classification in subghz_protocol_decoder_psa_feed():
//   ShortLow  <- (high, te_short)   ShortHigh <- (low, te_short)
//   LongLow   <- (high, te_long)    LongHigh  <- (low, te_long)
static void psa_encoder_emit_event(
    SubGhzProtocolEncoderPSA* instance,
    size_t* index,
    ManchesterEvent event) {
    bool level = false;
    uint32_t duration = 0;
    switch(event) {
    case ManchesterEventShortLow:
        level = true;
        duration = subghz_protocol_psa_const.te_short;
        break;
    case ManchesterEventShortHigh:
        level = false;
        duration = subghz_protocol_psa_const.te_short;
        break;
    case ManchesterEventLongLow:
        level = true;
        duration = subghz_protocol_psa_const.te_long;
        break;
    case ManchesterEventLongHigh:
        level = false;
        duration = subghz_protocol_psa_const.te_long;
        break;
    default:
        return;
    }
    if(*index < PSA_UPLOAD_CAPACITY) {
        instance->encoder.upload[(*index)++] = level_duration_make(level, duration);
    }
}

// Drive a mirror copy of the decoder's manchester_advance() FSM so it outputs
// exactly one bit `bit`. Tries a single event, else a priming (non-yielding)
// event followed by a yielding one. This is the guaranteed inverse of the
// decoder's Manchester recovery.
static bool psa_encoder_put_bit(
    SubGhzProtocolEncoderPSA* instance,
    size_t* index,
    ManchesterState* state,
    bool bit) {
    static const ManchesterEvent events[4] = {
        ManchesterEventShortLow,
        ManchesterEventShortHigh,
        ManchesterEventLongLow,
        ManchesterEventLongHigh};
    for(int i = 0; i < 4; i++) {
        ManchesterState next;
        bool data;
        if(manchester_advance(*state, events[i], &next, &data) && data == bit) {
            psa_encoder_emit_event(instance, index, events[i]);
            *state = next;
            return true;
        }
    }
    for(int i = 0; i < 4; i++) {
        ManchesterState mid;
        bool d0;
        if(manchester_advance(*state, events[i], &mid, &d0)) continue;
        for(int j = 0; j < 4; j++) {
            ManchesterState next;
            bool data;
            if(manchester_advance(mid, events[j], &next, &data) && data == bit) {
                psa_encoder_emit_event(instance, index, events[i]);
                psa_encoder_emit_event(instance, index, events[j]);
                *state = next;
                return true;
            }
        }
    }
    return false;
}

// Preamble low-shorts required by the decoder's State1 (pattern_counter must
// exceed PSA_PATTERN_THRESHOLD_1 = 0x46 = 70); 75 leaves margin.
#define PSA_ENC_PREAMBLE_PAIRS 75

// Rebuild the exact captured on-air frame: preamble + 64-bit Key1 + 16-bit
// validation field, Manchester-encoded the way the decoder reads it. No cipher
// is re-run -- the stored (already-encrypted) frame bits are reproduced as-is,
// so re-decoding yields the same Key1/Key2 (a verbatim clone of the capture).
static void psa_encoder_build_upload(SubGhzProtocolEncoderPSA* instance) {
    furi_check(instance);
    size_t index = 0;
    const uint32_t te_short = subghz_protocol_psa_const.te_short;
    const uint32_t te_long = subghz_protocol_psa_const.te_long;

    // State0 trigger: a single SHORT HIGH pulse arms State1.
    instance->encoder.upload[index++] = level_duration_make(true, te_short);
    // State1 preamble: SHORT pairs; each low short increments pattern_counter.
    for(int p = 0; p < PSA_ENC_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(false, te_short);
        instance->encoder.upload[index++] = level_duration_make(true, te_short);
    }
    // A LONG LOW pulse ends State1 and enters State2 (which resets Manchester).
    instance->encoder.upload[index++] = level_duration_make(false, te_long);
    // Sync trailing half-cell: a single SHORT HIGH. State2 starts from Mid1, where
    // a HIGH pulse maps to ShortLow -- a no-bit self-reset (state stays Mid1,
    // decode_count_bit unchanged). It is required physically: the first State2 data
    // symbol from Mid1 always OPENS with a LOW pulse, so without this HIGH the
    // te_long LOW marker would sit directly against that LOW -> two same-level
    // entries a real radio merges into one corrupted pulse. Emitting this HIGH makes
    // the State1->State2 boundary a genuine level transition with no bit change.
    if(index < PSA_UPLOAD_CAPACITY) {
        instance->encoder.upload[index++] = level_duration_make(true, te_short);
    }

    // State2 Manchester data: 64 Key1 bits then 16 validation bits, MSB first.
    ManchesterState state = ManchesterStateMid1;
    uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    for(int b = 63; b >= 0; b--) {
        psa_encoder_put_bit(instance, &index, &state, (key1 >> b) & 1);
    }
    for(int b = 15; b >= 0; b--) {
        psa_encoder_put_bit(instance, &index, &state, (instance->validation_field >> b) & 1);
    }

    // End-marker transition guard: the final Manchester symbol ends HIGH or LOW
    // depending on the last data bit, and the END marker below is HIGH. When the
    // last data pulse is also HIGH the two would sit adjacent and a real radio merges
    // them into one over-long HIGH -> the last bit and the END are both corrupted.
    // Emit a LOW te_short half-cell first in that case (at decode_count_bit == 80 the
    // decoder swallows it without consuming a bit), so the data->END boundary is
    // always a genuine level transition. When the last pulse is already LOW the END
    // HIGH is a clean transition and no filler is needed.
    if(index > 0 && level_duration_get_level(instance->encoder.upload[index - 1]) &&
       index < PSA_UPLOAD_CAPACITY) {
        instance->encoder.upload[index++] = level_duration_make(false, te_short);
    }

    // End marker: HIGH ~1000us finalizes the frame in State2.
    if(index < PSA_UPLOAD_CAPACITY) {
        instance->encoder.upload[index++] = level_duration_make(true, PSA_TE_END_1000);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
    instance->encoder.repeat = 10;
}

void* subghz_protocol_encoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPSA* instance = calloc(1, sizeof(SubGhzProtocolEncoderPSA));

    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolEncoderPSA));
        instance->base.protocol = &psa_protocol;
        instance->generic.protocol_name = instance->base.protocol->name;

        pp_encoder_buffer_ensure(instance, PSA_UPLOAD_CAPACITY);
        instance->encoder.repeat = 10;
        instance->encoder.front = 0;
        instance->encoder.is_running = false;
        instance->is_running = false;
    }

    return instance;
}

void subghz_protocol_encoder_psa_free(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    free(instance);
}

// Exact inverse of psa_second_stage_xor_decrypt. The decrypt derives plaintext
// buffer[2..7] from the encrypted bytes E2..E9 (E = buffer[2..9]) via:
//   P2 = E5^E7, P3 = E3^E5, P4 = E7^E2, P5 = E6^E4, P6 = E2^E4, P7 = E7^E9^E8
// buffer[8],buffer[9] (E8,E9) are never modified, so given plaintext P2..P7 the
// encrypted bytes are uniquely recoverable:
//   E7 = P7^E9^E8; E2 = P4^E7; E5 = P2^E7; E3 = P3^E5; E4 = P6^E2; E6 = P5^E4
// Proven identity: decrypt then encrypt (and vice-versa) restores buffer[2..9]
// (host unit test over all 256 seeds).
static void psa_second_stage_xor_encrypt(uint8_t* buffer) {
    const uint8_t E8 = buffer[8];
    const uint8_t E9 = buffer[9];
    const uint8_t P2 = buffer[2];
    const uint8_t P3 = buffer[3];
    const uint8_t P4 = buffer[4];
    const uint8_t P5 = buffer[5];
    const uint8_t P6 = buffer[6];
    const uint8_t P7 = buffer[7];
    const uint8_t E7 = (uint8_t)(P7 ^ E9 ^ E8);
    const uint8_t E2 = (uint8_t)(P4 ^ E7);
    const uint8_t E5 = (uint8_t)(P2 ^ E7);
    const uint8_t E3 = (uint8_t)(P3 ^ E5);
    const uint8_t E4 = (uint8_t)(P6 ^ E2);
    const uint8_t E6 = (uint8_t)(P5 ^ E4);
    buffer[2] = E2;
    buffer[3] = E3;
    buffer[4] = E4;
    buffer[5] = E5;
    buffer[6] = E6;
    buffer[7] = E7;
}

// Rolling re-encode for direct-XOR (Type 0x23) frames: recover the plaintext block
// from the stored Key1/Key2 via the SAME decode transform the decoder used, splice
// the requested rolling number into the counter bytes (decrypted buffer[5]=high,
// buffer[6]=low -- exactly where psa_extract_fields_mode23 reads counter =
// buffer[6] | buffer[5]<<8), then run the INVERSE transform to rebuild Key1. With
// the CAPTURED counter this is an exact identity, reproducing the stored value
// byte-for-byte; a bumped counter yields the next hop. Key1's top two bytes
// (buffer[0],buffer[1]) and Key2 (buffer[8],buffer[9]) are outside the XOR network
// and are preserved, so the decoder's 0xA device-class gate still passes.
static void psa_reencode_with_cnt(SubGhzProtocolEncoderPSA* instance, uint32_t cnt) {
    uint8_t buffer[PSA_BUFFER_SIZE] = {0};
    psa_crypto_setup_byte_buffer(
        buffer, instance->key1_low, instance->key1_high, instance->key2_low);
    psa_second_stage_xor_decrypt(buffer);
    buffer[6] = (uint8_t)(cnt & 0xFF);
    buffer[5] = (uint8_t)((cnt >> 8) & 0xFF);
    psa_second_stage_xor_encrypt(buffer);
    instance->key1_high = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                          ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
    instance->key1_low = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                         ((uint32_t)buffer[6] << 8) | (uint32_t)buffer[7];
}

SubGhzProtocolStatus
    subghz_protocol_encoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    // A faithful clone only needs the raw on-air fields the decoder always
    // serializes: Key1 (64-bit) and Key_2 (validation). Serial/Btn/Cnt/Type/CRC
    // are ONLY present when a frame decrypted at capture time, so they must be
    // optional here -- otherwise non-decrypted (e.g. TEA/0x36) captures, whose
    // serialize omits them, would fail to re-emit.
    uint64_t key1 = 0;
    if(!pp_flipper_read_hex_u64(flipper_format, FF_KEY, &key1)) {
        return SubGhzProtocolStatusError;
    }
    instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFFU);
    instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFFU);

    uint64_t key2 = 0;
    if(!pp_flipper_read_hex_u64(flipper_format, "Key_2", &key2)) {
        return SubGhzProtocolStatusError;
    }
    instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFFU);
    instance->validation_field = (uint16_t)(key2 & 0xFFFFU);

    // Rolling re-encode: direct-XOR (Type 0x23) captures serialize a decoded Cnt.
    // Re-derive Key1 with the (possibly harness-bumped) counter spliced in -- exact
    // identity for the captured counter (byte-for-byte clone), next hop for +1.
    // Undecrypted (0x36 / no-Cnt) captures skip this and are cloned verbatim.
    uint32_t type_val = 0;
    uint32_t cnt_val = UINT32_MAX;
    pp_encoder_read_fields(flipper_format, NULL, NULL, &cnt_val, &type_val);
    if(type_val == 0x23 && cnt_val != UINT32_MAX) {
        psa_reencode_with_cnt(instance, cnt_val);
    }

    psa_encoder_build_upload(instance);
    // build_upload hardcodes repeat = 10; honor a capture's Repeat if present.
    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);
    instance->is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_psa_stop(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;
    instance->is_running = false;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_psa_yield(void* context) {
    furi_check(context);
    SubGhzProtocolEncoderPSA* instance = context;

    if(!instance->is_running || instance->encoder.size_upload == 0) {
        instance->is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    instance->encoder.front++;

    if(instance->encoder.front >= instance->encoder.size_upload) {
        instance->encoder.front = 0;
        instance->encoder.repeat--;
        if(instance->encoder.repeat <= 0) {
            instance->is_running = false;
        }
    }

    return ret;
}
#endif
static uint32_t psa_abs_diff(uint32_t a, uint32_t b) {
    if(a < b) {
        return b - a;
    } else {
        return a - b;
    }
}

static bool psa_direct_xor_allowed_by_key2(uint8_t key2_high_byte) {
    uint8_t lo = key2_high_byte & 0xf;
    if(lo < 3) return true;
    if(lo < 7 && (key2_high_byte & 0xc) != 0) return true;
    return false;
}

static void psa_calculate_checksum(uint8_t* buffer) {
    uint32_t checksum = 0;
    for(int i = 2; i < 8; i++) {
        checksum += (buffer[i] & 0xF) + ((buffer[i] >> 4) & 0xF);
    }
    buffer[11] = (uint8_t)((checksum * 0x10) & 0xFF);
}

static void psa_copy_reverse(uint8_t* temp, uint8_t* source) {
    temp[0] = source[5];
    temp[1] = source[4];
    temp[2] = source[3];
    temp[3] = source[2];
    temp[4] = source[9];
    temp[5] = source[8];
    temp[6] = source[7];
    temp[7] = source[6];
}

static void psa_second_stage_xor_decrypt(uint8_t* buffer) {
    uint8_t temp[8];
    psa_copy_reverse(temp, buffer);
    buffer[2] = temp[0] ^ temp[6];
    buffer[3] = temp[2] ^ temp[0];
    buffer[4] = temp[6] ^ temp[3];
    buffer[5] = temp[7] ^ temp[1];
    buffer[6] = temp[3] ^ temp[1];
    buffer[7] = temp[6] ^ temp[4] ^ temp[5];
}

static void psa_extract_fields_mode23(uint8_t* buffer, SubGhzProtocolDecoderPSA* instance) {
    instance->decrypted_button = buffer[8] & 0xF;
    instance->decrypted_serial = ((uint32_t)buffer[3] << 8) | ((uint32_t)buffer[2] << 16) |
                                 (uint32_t)buffer[4];
    instance->decrypted_counter = (uint32_t)buffer[6] | ((uint32_t)buffer[5] << 8);
    instance->decrypted_crc = (uint16_t)buffer[7];
    instance->decrypted_type = 0x23;
    instance->decrypted_seed = instance->decrypted_serial;
}

static bool psa_direct_xor_decrypt(SubGhzProtocolDecoderPSA* instance, uint8_t* buffer) {
    psa_calculate_checksum(buffer);
    uint8_t checksum = buffer[11];
    uint8_t key2_high = buffer[8];

    uint8_t validation_result = (checksum ^ key2_high) & 0xF0;
    FURI_LOG_D(
        TAG,
        "Direct XOR validation: checksum=0x%02X, key2_high=0x%02X, result=0x%02X",
        checksum,
        key2_high,
        validation_result);

    if(validation_result == 0) {
        buffer[13] = buffer[9] ^ buffer[8];
        psa_second_stage_xor_decrypt(buffer);
        psa_extract_fields_mode23(buffer, instance);
        FURI_LOG_D(TAG, "Direct XOR decryption completed");
        return true;
    }
    FURI_LOG_D(
        TAG,
        "Direct XOR validation FAILED: (checksum ^ key2_high) & 0xF0 = 0x%02X (expected 0x00)",
        validation_result);
    return false;
}

static void psa_handle_decoded_frame(SubGhzProtocolDecoderPSA* instance, uint8_t direct_xor_mode) {
    instance->key2_low = instance->decode_data_low;
    instance->key2_high = instance->decode_data_high;
    instance->validation_field = (uint16_t)(instance->decode_data_low & 0xFFFF);
    instance->decrypted_type = 0;
    instance->decrypted_button = 0;
    instance->decrypted_serial = 0;
    instance->decrypted_counter = 0;
    instance->decrypted_crc = 0;
    instance->decrypted_seed = 0;
    instance->decrypted = 0x00;
    instance->mode_serialize = psa_direct_xor_allowed_by_key2((uint8_t)(instance->key2_low >> 8)) ?
                                   direct_xor_mode :
                                   0x36;
    instance->status_flag = 0x80;

    FURI_LOG_I(
        TAG,
        "Signal decoded - Key1:%08lX%08lX Key2:%04X ValField:%04X",
        (unsigned long)instance->key1_high,
        (unsigned long)instance->key1_low,
        (unsigned int)(instance->key2_low & 0xFFFF),
        (unsigned int)instance->validation_field);

    uint8_t buffer[PSA_BUFFER_SIZE] = {0};
    psa_crypto_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);
    if(instance->mode_serialize != 0x36 && psa_direct_xor_decrypt(instance, buffer)) {
        instance->mode_serialize = 0x23;
        instance->decrypted = 0x50;
        FURI_LOG_I(
            TAG,
            "Direct XOR decryption SUCCESS - Type:0x23 Btn:%02X Ser:%06lX Cnt:%08lX",
            (unsigned int)instance->decrypted_button,
            (unsigned long)instance->decrypted_serial,
            (unsigned long)instance->decrypted_counter);
    } else {
        instance->decrypted = 0x00;
        instance->mode_serialize = 0x36;
        FURI_LOG_I(
            TAG,
            "Direct XOR decryption FAILED - marked for brute force (Type:0x36) - deferred to get_string");
    }
    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    instance->decode_data_low = 0;
    instance->decode_data_high = 0;
    instance->decode_count_bit = 0;
}

static void psa_decrypt_router(SubGhzProtocolDecoderPSA* instance) {
    FURI_LOG_I(TAG, "=== DECRYPTION ROUTER CALLED ===");
    FURI_LOG_I(
        TAG,
        "Key1:%08lX%08lX Key2:%04X mode_serialize:0x%02X decrypted:0x%02X",
        (unsigned long)instance->key1_high,
        (unsigned long)instance->key1_low,
        (unsigned int)(instance->key2_low & 0xFFFF),
        (unsigned int)instance->mode_serialize,
        (unsigned int)instance->decrypted);

    uint8_t buffer[PSA_BUFFER_SIZE] = {0};

    psa_crypto_setup_byte_buffer(buffer, instance->key1_low, instance->key1_high, instance->key2_low);

    uint8_t key2_high_byte = buffer[8];
    uint8_t mode = instance->mode_serialize;

    if(mode == 1 || mode == 2) {
        if(!psa_direct_xor_allowed_by_key2(key2_high_byte)) {
            mode = 0x36;
            FURI_LOG_I(
                TAG,
                "Key2 high byte 0x%02X fails OG direct-XOR filter -> 0x36 (TEA BF)",
                key2_high_byte);
        } else if(psa_direct_xor_decrypt(instance, buffer)) {
            mode = 0x23;
            FURI_LOG_I(TAG, "Converted mode %d -> 0x23 (Direct XOR)", instance->mode_serialize);
        } else {
            mode = 0x36;
            FURI_LOG_I(
                TAG, "Converted mode %d -> 0x36 (TEA Brute Force)", instance->mode_serialize);
        }
    }

    if(mode == 0x23) {
        FURI_LOG_I(TAG, "Attempting Direct XOR decryption (Type 0x23)");
        if(psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            FURI_LOG_I(TAG, "Direct XOR decryption SUCCESS");
            return;
        }
        FURI_LOG_W(TAG, "Direct XOR decryption FAILED");
    } else if(mode == 0x36) {
        FURI_LOG_I(TAG, "TEA BF deferred - use Brute force in receiver info");
        return;
    } else {
        FURI_LOG_I(TAG, "Mode uninitialized (0x00) - trying direct XOR or BF");
        if(psa_direct_xor_allowed_by_key2(key2_high_byte) &&
           psa_direct_xor_decrypt(instance, buffer)) {
            instance->mode_serialize = 0x23;
            instance->decrypted = 0x50;
            FURI_LOG_I(TAG, "Direct XOR decryption SUCCESS");
            return;
        }

        FURI_LOG_I(TAG, "Direct XOR skipped or failed - TEA BF deferred to plugin");
        instance->mode_serialize = 0x36;
        instance->decrypted = 0x00;
        return;
    }

    FURI_LOG_E(TAG, "=== ALL DECRYPTION ATTEMPTS FAILED ===");
    instance->decrypted = 0x00;
}

void* subghz_protocol_decoder_psa_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPSA* instance = calloc(1, sizeof(SubGhzProtocolDecoderPSA));
    if(instance) {
        memset(instance, 0, sizeof(SubGhzProtocolDecoderPSA));
        instance->base.protocol = &psa_protocol;
        instance->manchester_state = ManchesterStateMid1;
    }
    return instance;
}

void subghz_protocol_decoder_psa_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;
    instance->state = 0;
    instance->status_flag = 0;
    instance->mode_serialize = 0;
    instance->key1_low = 0;
    instance->key1_high = 0;
    instance->key2_low = 0;
    instance->key2_high = 0;
    instance->decode_data_low = 0;
    instance->decode_data_high = 0;
    instance->decode_count_bit = 0;
    instance->pattern_counter = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->decrypted_button = 0;
    instance->decrypted_serial = 0;
    instance->decrypted_counter = 0;
    instance->decrypted_crc = 0;
    instance->decrypted_seed = 0;
    instance->decrypted_type = 0;
}

void subghz_protocol_decoder_psa_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    uint32_t tolerance;
    uint32_t new_state = instance->state;
    uint32_t prev_dur = instance->prev_duration;
    uint32_t te_short = subghz_protocol_psa_const.te_short;
    uint32_t te_long = subghz_protocol_psa_const.te_long;

    switch(instance->state) {
    case PSADecoderState0:
        if(!level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance > PSA_TOLERANCE_99) {
                if(duration < PSA_TE_SHORT_125) {
                    tolerance = PSA_TE_SHORT_125 - duration;
                } else {
                    tolerance = duration - PSA_TE_SHORT_125;
                }
                if(tolerance > 40) {
                    return;
                }
                if(duration > 180) {
                    return;
                }
                new_state = PSADecoderState3;
            } else {
                new_state = PSADecoderState1;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance > PSA_TOLERANCE_99) {
                return;
            }
            new_state = PSADecoderState1;
        }

        instance->decode_data_low = 0;
        instance->decode_data_high = 0;
        instance->pattern_counter = 0;
        instance->decode_count_bit = 0;
        instance->mode_serialize = 0;
        instance->prev_duration = duration;
        instance->decrypted_type = 0;
        instance->decrypted_button = 0;
        instance->decrypted_serial = 0;
        instance->decrypted_counter = 0;
        instance->decrypted_crc = 0;
        instance->decrypted_seed = 0;
        instance->decrypted = 0x00;
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        break;

    case PSADecoderState1:
        if(level) {
            return;
        }

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            }
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                uint32_t prev_diff = psa_abs_diff(prev_dur, te_short);
                if(prev_diff <= PSA_TOLERANCE_99) {
                    instance->pattern_counter++;
                }
                instance->prev_duration = duration;
                return;
            } else {
                uint32_t long_diff;
                if(duration < te_long) {
                    long_diff = te_long - duration;
                } else {
                    long_diff = duration - te_long;
                }
                if(long_diff < 100) {
                    if(instance->pattern_counter > PSA_PATTERN_THRESHOLD_1) {
                        FURI_LOG_I(
                            TAG,
                            "[State1->State2] Transition detected with pattern_cnt=%lu",
                            (unsigned long)instance->pattern_counter);
                        new_state = PSADecoderState2;
                        instance->decode_data_low = 0;
                        instance->decode_data_high = 0;
                        instance->decode_count_bit = 0;
                        manchester_advance(
                            instance->manchester_state,
                            ManchesterEventReset,
                            &instance->manchester_state,
                            NULL);
                        instance->state = new_state;
                    }
                    instance->pattern_counter = 0;
                    instance->prev_duration = duration;
                    return;
                }
            }
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState2:
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        if(level && instance->decode_count_bit == PSA_KEY2_BITS) {
            if(duration >= 800) {
                uint32_t end_diff;
                if(duration < PSA_TE_END_1000) {
                    end_diff = PSA_TE_END_1000 - duration;
                } else {
                    end_diff = duration - PSA_TE_END_1000;
                }
                if(end_diff <= 199) {
                    if(((instance->key1_high >> 16) & 0xF) != 0xA) {
                        instance->decode_data_low = 0;
                        instance->decode_data_high = 0;
                        instance->decode_count_bit = 0;
                        new_state = PSADecoderState0;
                        instance->state = new_state;
                        return;
                    }
                    psa_handle_decoded_frame(instance, 1);
                    new_state = PSADecoderState0;
                    instance->state = new_state;
                    return;
                }
            }
        }

        uint8_t manchester_input = 0;
        bool should_process = false;

        if(duration < te_short) {
            tolerance = te_short - duration;
            if(tolerance >= PSA_TOLERANCE_100) {
                return;
            }
            manchester_input = ((level ^ 1) & 0x7f) << 1;
            should_process = true;
        } else {
            tolerance = duration - te_short;
            if(tolerance < PSA_TOLERANCE_100) {
                manchester_input = ((level ^ 1) & 0x7f) << 1;
                should_process = true;
            } else if(duration < te_long) {
                uint32_t diff_from_250 = duration - te_short;
                uint32_t diff_from_500 = te_long - duration;

                if(diff_from_500 < 150 || diff_from_250 > diff_from_500) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else if(diff_from_250 < 150) {
                    manchester_input = ((level ^ 1) & 0x7f) << 1;
                    should_process = true;
                } else {
                    if(duration > 10000) {
                        new_state = PSADecoderState0;
                        instance->pattern_counter = 0;
                        return;
                    }
                    if(duration >= 350 && duration <= 400) {
                        if(level == 0) {
                            manchester_input = 6;
                        } else {
                            manchester_input = 4;
                        }
                        should_process = true;
                    } else {
                        return;
                    }
                }
            } else {
                uint32_t long_diff = duration - te_long;
                if(long_diff < 100) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                    should_process = true;
                } else {
                    if(!level) {
                        if(duration > 10000) {
                            new_state = PSADecoderState0;
                            instance->pattern_counter = 0;
                            return;
                        }
                        return;
                    }
                    should_process = false;
                }
            }
        }

        if(should_process && instance->decode_count_bit < PSA_KEY2_BITS) {
            bool decoded_bit = false;

            if(manchester_advance(
                   instance->manchester_state,
                   (ManchesterEvent)manchester_input,
                   &instance->manchester_state,
                   &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                instance->decode_data_low = (instance->decode_data_low << 1) |
                                            (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                }
            }
        }

        if(!level) {
            return;
        }

        if(!should_process) {
            uint32_t end_diff;
            if(duration < PSA_TE_END_1000) {
                end_diff = PSA_TE_END_1000 - duration;
            } else {
                end_diff = duration - PSA_TE_END_1000;
            }
            if(end_diff <= 199) {
                if(instance->decode_count_bit != PSA_KEY2_BITS) {
                    return;
                }

                if(((instance->key1_high >> 16) & 0xF) == 0xA) {
                    psa_handle_decoded_frame(instance, 1);
                    new_state = PSADecoderState0;
                    instance->state = new_state;
                    return;
                } else {
                    return;
                }
            } else {
                return;
            }
        }
        break;

    case PSADecoderState3:
        if(duration >= 250) {
            if(duration >= PSA_TE_LONG_250 && duration < PSA_TE_LONG_300) {
                if(instance->pattern_counter > PSA_PATTERN_THRESHOLD_2) {
                    FURI_LOG_I(
                        TAG,
                        "[State3->State4] Transition detected with pattern_cnt=%lu",
                        (unsigned long)instance->pattern_counter);
                    new_state = PSADecoderState4;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                    instance->decode_count_bit = 0;
                    manchester_advance(
                        instance->manchester_state,
                        ManchesterEventReset,
                        &instance->manchester_state,
                        NULL);
                    instance->state = new_state;
                    instance->pattern_counter = 0;
                    instance->prev_duration = duration;
                    return;
                }
            }
            new_state = PSADecoderState0;
            instance->pattern_counter = 0;
            break;
        }

        if(duration < PSA_TE_SHORT_125) {
            tolerance = PSA_TE_SHORT_125 - duration;
        } else {
            tolerance = duration - PSA_TE_SHORT_125;
        }

        if(tolerance < PSA_TOLERANCE_50) {
            uint32_t prev_diff = psa_abs_diff(prev_dur, PSA_TE_SHORT_125);
            if(prev_diff <= PSA_TOLERANCE_49) {
                instance->pattern_counter++;
            } else {
                instance->pattern_counter = 0;
            }
            instance->prev_duration = duration;
            return;
        }

        new_state = PSADecoderState0;
        instance->pattern_counter = 0;
        break;

    case PSADecoderState4:
        if(instance->decode_count_bit >= PSA_MAX_BITS) {
            new_state = PSADecoderState0;
            break;
        }

        if(!level) {
            uint8_t manchester_input;
            bool decoded_bit = false;

            if(duration < PSA_TE_SHORT_125) {
                tolerance = PSA_TE_SHORT_125 - duration;
                if(tolerance > PSA_TOLERANCE_49) {
                    return;
                }
                manchester_input = ((level ^ 1) & 0x7f) << 1;
            } else {
                tolerance = duration - PSA_TE_SHORT_125;
                if(tolerance < PSA_TOLERANCE_50) {
                    manchester_input = ((level ^ 1) & 0x7f) << 1;
                } else if(duration >= PSA_TE_LONG_250 && duration < PSA_TE_LONG_300) {
                    if(level == 0) {
                        manchester_input = 6;
                    } else {
                        manchester_input = 4;
                    }
                } else {
                    return;
                }
            }

            if(manchester_advance(
                   instance->manchester_state,
                   (ManchesterEvent)manchester_input,
                   &instance->manchester_state,
                   &decoded_bit)) {
                uint32_t carry = (instance->decode_data_low >> 31) & 1;
                instance->decode_data_low = (instance->decode_data_low << 1) |
                                            (decoded_bit ? 1 : 0);
                instance->decode_data_high = (instance->decode_data_high << 1) | carry;
                instance->decode_count_bit++;

                if(instance->decode_count_bit == PSA_KEY1_BITS) {
                    instance->key1_low = instance->decode_data_low;
                    instance->key1_high = instance->decode_data_high;
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                }
            }
        } else if(level) {
            uint32_t end_diff;
            if(duration < PSA_TE_END_500) {
                end_diff = PSA_TE_END_500 - duration;
            } else {
                end_diff = duration - PSA_TE_END_500;
            }
            if(end_diff <= 99) {
                if(instance->decode_count_bit != PSA_KEY2_BITS) {
                    return;
                }

                if(((instance->key1_high >> 16) & 0xF) != 0xA) {
                    instance->decode_data_low = 0;
                    instance->decode_data_high = 0;
                    instance->decode_count_bit = 0;
                    new_state = PSADecoderState0;
                    instance->state = new_state;
                    return;
                }
                psa_handle_decoded_frame(instance, 2);
                new_state = PSADecoderState0;
                instance->state = new_state;
                return;
            } else {
                return;
            }
        }
        break;
    }

    instance->state = new_state;
    instance->prev_duration = duration;
}

uint8_t subghz_protocol_decoder_psa_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;
    uint64_t combined_data = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
    SubGhzBlockDecoder decoder = {.decode_data = combined_data, .decode_count_bit = 64};
    return subghz_protocol_blocks_get_hash_data(&decoder, 16);
}

SubGhzProtocolStatus subghz_protocol_decoder_psa_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        if(!flipper_format_write_uint32(flipper_format, FF_FREQUENCY, &preset->frequency, 1))
            break;

        if(!flipper_format_write_string_cstr(
               flipper_format, FF_PRESET, furi_string_get_cstr(preset->name)))
            break;

        if(!flipper_format_write_string_cstr(
               flipper_format, FF_PROTOCOL, instance->base.protocol->name))
            break;

        uint32_t bits = 128;
        if(!flipper_format_write_uint32(flipper_format, FF_BIT, &bits, 1)) break;

        char key1_str[20];
        uint64_t key1 = ((uint64_t)instance->key1_high << 32) | instance->key1_low;
        snprintf(key1_str, sizeof(key1_str), "%016llX", key1);
        if(!flipper_format_write_string_cstr(flipper_format, FF_KEY, key1_str)) break;

        char key2_str[20];
        uint64_t key2 = ((uint64_t)instance->key2_high << 32) | instance->key2_low;
        snprintf(key2_str, sizeof(key2_str), "%016llX", key2);
        if(!flipper_format_write_string_cstr(flipper_format, "Key_2", key2_str)) break;

        uint32_t val_field = instance->validation_field;
        flipper_format_write_uint32(flipper_format, "ValidationField", &val_field, 1);

        if(instance->decrypted == 0x50 && instance->decrypted_type != 0) {
            pp_serialize_fields(
                flipper_format,
                PP_FIELD_SERIAL | PP_FIELD_BTN | PP_FIELD_CNT | PP_FIELD_TYPE,
                instance->decrypted_serial,
                instance->decrypted_button,
                instance->decrypted_counter,
                instance->decrypted_type);

            uint32_t crc_temp = instance->decrypted_crc;
            flipper_format_write_uint32(flipper_format, "CRC", &crc_temp, 1);

            flipper_format_write_uint32(flipper_format, "Seed", &instance->decrypted_seed, 1);
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_psa_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        uint64_t key1 = 0;
        if(!pp_flipper_read_hex_u64(flipper_format, FF_KEY, &key1)) break;
        instance->key1_low = (uint32_t)(key1 & 0xFFFFFFFF);
        instance->key1_high = (uint32_t)((key1 >> 32) & 0xFFFFFFFF);

        uint64_t key2 = 0;
        if(!pp_flipper_read_hex_u64(flipper_format, "Key_2", &key2)) break;
        instance->key2_low = (uint32_t)(key2 & 0xFFFFFFFF);
        instance->key2_high = (uint32_t)((key2 >> 32) & 0xFFFFFFFF);

        instance->status_flag = 0x80;

        psa_decrypt_router(instance);

        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

void subghz_protocol_decoder_psa_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderPSA* instance = context;

    if(instance->status_flag == 0x80 && (instance->key1_low != 0 || instance->key1_high != 0) &&
       instance->decrypted_type == 0) {
        FURI_LOG_I(TAG, "get_string: Calling decryption router (decrypted_type=0)");
        psa_decrypt_router(instance);
    } else {
        FURI_LOG_D(
            TAG,
            "get_string: Skipping router - status_flag=0x%08lX decrypted_type=0x%02X",
            (unsigned long)instance->status_flag,
            (unsigned int)instance->decrypted_type);
    }

    uint16_t key2_value = (uint16_t)(instance->key2_low & 0xFFFF);

    if(instance->decrypted == 0x50 && instance->decrypted_type != 0) {
        if(instance->decrypted_type == 0x23) {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Key1:%08lX%08lX\r\n"
                "Key2:%04X\r\n"
                "Btn:%01X\r\n"
                "Ser:%06lX\r\n"
                "Cnt:%04lX\r\n"
                "CRC:%02X\r\n"
                "Type:%02X\r\n"
                "Sd:%06lX",
                instance->base.protocol->name,
                128,
                instance->key1_high,
                instance->key1_low,
                key2_value,
                instance->decrypted_button,
                instance->decrypted_serial,
                (uint32_t)instance->decrypted_counter,
                instance->decrypted_crc,
                instance->decrypted_type,
                instance->decrypted_seed);
        } else if(instance->decrypted_type == 0x36) {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Key1:%08lX%08lX\r\n"
                "Key2:%04X\r\n"
                "Btn:%02X\r\n"
                "Ser:%06lX\r\n"
                "Cnt:%lX\r\n"
                "CRC:%02X\r\n"
                "Type:%02X\r\n"
                "Sd:%06lX",
                instance->base.protocol->name,
                128,
                instance->key1_high,
                instance->key1_low,
                key2_value,
                instance->decrypted_button,
                instance->decrypted_serial,
                instance->decrypted_counter,
                instance->decrypted_crc,
                instance->decrypted_type,
                instance->decrypted_seed);
        } else {
            furi_string_printf(
                output,
                "%s %dbit\r\n"
                "Key1:%08lX%08lX\r\n"
                "Key2:%X",
                instance->base.protocol->name,
                128,
                instance->key1_high,
                instance->key1_low,
                key2_value);
        }
    } else {
        furi_string_printf(
            output,
            "%s %dbit\r\n"
            "Key1:%08lX%08lX\r\n"
            "Key2:%X",
            instance->base.protocol->name,
            128,
            instance->key1_high,
            instance->key1_low,
            key2_value);
    }
}
