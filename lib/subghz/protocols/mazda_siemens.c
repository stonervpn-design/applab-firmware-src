#include "mazda_siemens.h"
#include "protocols_common.h"

// Ported from KAT src/protocols/mazda_siemens.rs
#define MZS_TE_SHORT       250
#define MZS_TE_LONG        500
#define MZS_TE_DELTA       100
#define MZS_BIT_COUNT      64
#define MZS_PREAMBLE_MIN   13
#define MZS_COMPLETION_MIN 80
#define MZS_COMPLETION_MAX 105
#define MZS_BUFFER_SIZE    14
#define MZS_PREAMBLE_BYTES 12
#define MZS_TX_GAP_US      50000

static const SubGhzBlockConst subghz_protocol_mazda_siemens_const = {
    .te_short = MZS_TE_SHORT,
    .te_long = MZS_TE_LONG,
    .te_delta = MZS_TE_DELTA,
    .min_count_bit_for_found = MZS_BIT_COUNT,
};

#ifdef ENABLE_EMULATE_FEATURE
// preamble(12*16) + gap + (3+8+1 bytes)*16 + gap
#define MZS_UPLOAD_CAPACITY ((MZS_PREAMBLE_BYTES * 16U) + 1U + (12U * 16U) + 1U)
_Static_assert(
    MZS_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "MZS_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

typedef enum {
    MzsDecoderStepReset = 0,
    MzsDecoderStepPreambleSave,
    MzsDecoderStepPreambleCheck,
    MzsDecoderStepDataSave,
    MzsDecoderStepDataCheck,
} MzsDecoderStep;

struct SubGhzProtocolDecoderMazdaSiemens {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te_last;
    uint16_t preamble_count;
    uint16_t bit_counter;
    uint8_t prev_state;
    uint8_t data_buffer[MZS_BUFFER_SIZE];
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderMazdaSiemens {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};
#endif

static bool mzs_is_short(uint32_t d) {
    return DURATION_DIFF(d, MZS_TE_SHORT) < MZS_TE_DELTA;
}
static bool mzs_is_long(uint32_t d) {
    return DURATION_DIFF(d, MZS_TE_LONG) < MZS_TE_DELTA;
}

static uint8_t mzs_byte_parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1U;
}

static void mzs_xor_deobfuscate(uint8_t* data) {
    uint8_t parity = mzs_byte_parity(data[7]);
    if(parity) {
        uint8_t mask = data[6];
        for(int i = 0; i < 6; i++) data[i] ^= mask;
    } else {
        uint8_t mask = data[5];
        for(int i = 0; i < 5; i++) data[i] ^= mask;
        data[6] ^= mask;
    }
    uint8_t old5 = data[5];
    uint8_t old6 = data[6];
    data[5] = (uint8_t)((old5 & 0xAA) | (old6 & 0x55));
    data[6] = (uint8_t)((old5 & 0x55) | (old6 & 0xAA));
}

#ifdef ENABLE_EMULATE_FEATURE
static void mzs_xor_obfuscate(uint8_t* data) {
    uint8_t old5 = data[5];
    uint8_t old6 = data[6];
    data[5] = (uint8_t)((old5 & 0xAA) | (old6 & 0x55));
    data[6] = (uint8_t)((old5 & 0x55) | (old6 & 0xAA));

    uint8_t parity = mzs_byte_parity(data[7]);
    if(parity) {
        uint8_t mask = data[6];
        for(int i = 0; i < 6; i++) data[i] ^= mask;
    } else {
        uint8_t mask = data[5];
        for(int i = 0; i < 5; i++) data[i] ^= mask;
        data[6] ^= mask;
    }
}
#endif

static void mzs_collect_bit(SubGhzProtocolDecoderMazdaSiemens* instance, uint8_t state_bit) {
    uint16_t byte_idx = instance->bit_counter >> 3;
    if(byte_idx < MZS_BUFFER_SIZE) {
        instance->data_buffer[byte_idx] <<= 1;
        if(state_bit == 0) {
            instance->data_buffer[byte_idx] |= 1U;
        }
    }
    instance->bit_counter++;
}

static bool mzs_process_pair(
    SubGhzProtocolDecoderMazdaSiemens* instance,
    uint32_t dur_first,
    uint32_t dur_second) {
    bool fs = mzs_is_short(dur_first);
    bool fl = mzs_is_long(dur_first);
    bool ss = mzs_is_short(dur_second);
    bool sl = mzs_is_long(dur_second);

    if(fl && ss) {
        mzs_collect_bit(instance, 0);
        mzs_collect_bit(instance, 1);
        instance->prev_state = 1;
        return true;
    }
    if(fs && sl) {
        mzs_collect_bit(instance, 1);
        instance->prev_state = 0;
        return true;
    }
    if(fs && ss) {
        mzs_collect_bit(instance, instance->prev_state);
        return true;
    }
    if(fl && sl) {
        mzs_collect_bit(instance, 0);
        mzs_collect_bit(instance, 1);
        instance->prev_state = 0;
        return true;
    }
    return false;
}

static void mzs_check_completion(SubGhzProtocolDecoderMazdaSiemens* instance) {
    if(instance->bit_counter < MZS_COMPLETION_MIN || instance->bit_counter > MZS_COMPLETION_MAX) {
        return;
    }

    uint8_t data[8];
    for(int i = 0; i < 8; i++) data[i] = instance->data_buffer[i + 1];

    mzs_xor_deobfuscate(data);

    uint8_t checksum = 0;
    for(int i = 0; i < 7; i++) checksum = (uint8_t)(checksum + data[i]);
    if(checksum != data[7]) {
        return;
    }

    uint64_t packed = 0;
    for(int i = 0; i < 8; i++) packed = (packed << 8) | data[i];

    instance->generic.data = packed;
    instance->generic.data_count_bit = MZS_BIT_COUNT;
    instance->generic.serial = (uint32_t)(packed >> 32);
    instance->generic.btn = (uint8_t)((packed >> 24) & 0xFF);
    instance->generic.cnt = (uint16_t)((packed >> 8) & 0xFFFF);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void* subghz_protocol_decoder_mazda_siemens_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMazdaSiemens* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderMazdaSiemens));
    furi_check(instance);
    instance->base.protocol = &mazda_siemens_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_mazda_siemens_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaSiemens* instance = context;
    instance->decoder.parser_step = MzsDecoderStepReset;
    instance->preamble_count = 0;
    instance->bit_counter = 0;
    instance->prev_state = 0;
    memset(instance->data_buffer, 0, sizeof(instance->data_buffer));
}

void subghz_protocol_decoder_mazda_siemens_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    UNUSED(level); // pair/duration-based decode, level-agnostic (matches KAT)
    SubGhzProtocolDecoderMazdaSiemens* instance = context;

    switch(instance->decoder.parser_step) {
    case MzsDecoderStepReset:
        if(mzs_is_short(duration)) {
            instance->te_last = duration;
            instance->preamble_count = 0;
            instance->decoder.parser_step = MzsDecoderStepPreambleCheck;
        }
        break;
    case MzsDecoderStepPreambleSave:
        instance->te_last = duration;
        instance->decoder.parser_step = MzsDecoderStepPreambleCheck;
        break;
    case MzsDecoderStepPreambleCheck:
        if(mzs_is_short(instance->te_last) && mzs_is_short(duration)) {
            instance->preamble_count++;
            instance->decoder.parser_step = MzsDecoderStepPreambleSave;
        } else if(
            mzs_is_short(instance->te_last) && mzs_is_long(duration) &&
            instance->preamble_count >= MZS_PREAMBLE_MIN) {
            instance->bit_counter = 1;
            memset(instance->data_buffer, 0, sizeof(instance->data_buffer));
            // bit_counter set to 1 then collect_bit writes at index (1>>3)=0; matches KAT
            instance->bit_counter = 0;
            mzs_collect_bit(instance, 1);
            instance->prev_state = 0;
            instance->decoder.parser_step = MzsDecoderStepDataSave;
        } else {
            instance->decoder.parser_step = MzsDecoderStepReset;
        }
        break;
    case MzsDecoderStepDataSave:
        instance->te_last = duration;
        instance->decoder.parser_step = MzsDecoderStepDataCheck;
        break;
    case MzsDecoderStepDataCheck:
        if(mzs_process_pair(instance, instance->te_last, duration)) {
            instance->decoder.parser_step = MzsDecoderStepDataSave;
        } else {
            mzs_check_completion(instance);
            instance->decoder.parser_step = MzsDecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_mazda_siemens_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaSiemens* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_mazda_siemens_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaSiemens* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN | PP_FIELD_CNT,
            instance->generic.serial,
            instance->generic.btn,
            instance->generic.cnt,
            0);
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_mazda_siemens_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaSiemens* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_mazda_siemens_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        uint64_t packed = instance->generic.data;
        instance->generic.serial = (uint32_t)(packed >> 32);
        instance->generic.btn = (uint8_t)((packed >> 24) & 0xFF);
        instance->generic.cnt = (uint16_t)((packed >> 8) & 0xFFFF);
    }
    return ret;
}

// Next-code predictor. The displayed/loadable Key is generic.data, i.e. the
// plaintext (deobfuscated) 64-bit frame, so no crypto is needed: the 16-bit BE
// rolling counter is cleartext at data[5..6] and is protected only by the
// additive checksum data[7]=sum(data[0..6]). This reuses the exact
// counter-advance + checksum-recompute already proven in mzs_build_upload
// (the XOR obfuscation there is applied only for RF TX and is irrelevant to
// the displayed/loadable Key). Byte-correct → predicted frames are
// load/emulate/save-able.
#define MZS_PREDICT_COUNT 3

static uint64_t mzs_predict_key(uint64_t frame, uint16_t add) {
    uint8_t data[8];
    for(int i = 0; i < 8; i++) data[i] = (uint8_t)((frame >> (56 - 8 * i)) & 0xFF);

    uint16_t cnt = (uint16_t)(((uint16_t)data[5] << 8) | data[6]);
    cnt = (uint16_t)(cnt + add);
    data[5] = (uint8_t)(cnt >> 8);
    data[6] = (uint8_t)(cnt & 0xFF);

    uint8_t checksum = 0;
    for(int i = 0; i < 7; i++) checksum = (uint8_t)(checksum + data[i]);
    data[7] = checksum;

    uint64_t packed = 0;
    for(int i = 0; i < 8; i++) packed = (packed << 8) | data[i];
    return packed;
}

void subghz_protocol_decoder_mazda_siemens_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMazdaSiemens* instance = context;
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%08lX Btn:%02X Cnt:%04X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF),
        (unsigned long)instance->generic.serial,
        (unsigned int)instance->generic.btn,
        (unsigned int)instance->generic.cnt);

    // Predicted future rolling codes (only when a valid frame is present).
    if(instance->generic.data_count_bit == MZS_BIT_COUNT) {
        for(uint16_t i = 1; i <= MZS_PREDICT_COUNT; i++) {
            uint64_t next_key = mzs_predict_key(instance->generic.data, i);
            furi_string_cat_printf(
                output, "\r\nNext+%u:%016llX", (unsigned int)i, (unsigned long long)next_key);
        }
    }
}

#ifdef ENABLE_EMULATE_FEATURE
static void mzs_encode_byte(LevelDuration* up, size_t* index, size_t cap, uint8_t byte) {
    for(int bit = 7; bit >= 0; bit--) {
        if((byte >> bit) & 1U) {
            *index = pp_emit(up, *index, cap, true, MZS_TE_SHORT);
            *index = pp_emit(up, *index, cap, false, MZS_TE_SHORT);
        } else {
            *index = pp_emit(up, *index, cap, false, MZS_TE_SHORT);
            *index = pp_emit(up, *index, cap, true, MZS_TE_SHORT);
        }
    }
}

static bool mzs_build_upload(SubGhzProtocolEncoderMazdaSiemens* instance, uint8_t button) {
    uint8_t data[8];
    for(int i = 0; i < 8; i++) data[i] = (uint8_t)((instance->generic.data >> (56 - 8 * i)) & 0xFF);

    // Counter comes straight from generic.data (deserialize set it to the exact
    // requested Cnt); apply button + recompute checksum. NO hidden +1 — the caller
    // supplies the target counter, so Next/Burst advance correctly (mirrors mazda_v0).
    data[4] = button;

    uint8_t checksum = 0;
    for(int i = 0; i < 7; i++) checksum = (uint8_t)(checksum + data[i]);
    data[7] = checksum;

    uint8_t tx[8];
    memcpy(tx, data, 8);
    mzs_xor_obfuscate(tx);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = MZS_UPLOAD_CAPACITY;
    size_t index = 0;

    for(int i = 0; i < MZS_PREAMBLE_BYTES; i++) mzs_encode_byte(up, &index, cap, 0xFF);
    index = pp_emit(up, index, cap, false, MZS_TX_GAP_US);

    mzs_encode_byte(up, &index, cap, 0xFF);
    mzs_encode_byte(up, &index, cap, 0xFF);
    mzs_encode_byte(up, &index, cap, 0xD7);
    for(int i = 0; i < 8; i++) mzs_encode_byte(up, &index, cap, (uint8_t)(255 - tx[i]));
    mzs_encode_byte(up, &index, cap, 0x5A);
    index = pp_emit(up, index, cap, false, MZS_TX_GAP_US);

    // keep generic.data consistent with the frame we just built
    uint64_t packed = 0;
    for(int i = 0; i < 8; i++) packed = (packed << 8) | data[i];
    instance->generic.data = packed;
    instance->generic.serial = (uint32_t)(packed >> 32);
    instance->generic.btn = (uint8_t)((packed >> 24) & 0xFF);
    instance->generic.cnt = (uint16_t)((packed >> 8) & 0xFFFF);

    if(index > cap) return false;
    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_mazda_siemens_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMazdaSiemens* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderMazdaSiemens));
    furi_check(instance);
    instance->base.protocol = &mazda_siemens_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    pp_encoder_buffer_ensure(instance, MZS_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_mazda_siemens_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderMazdaSiemens* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }
    SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, MZS_BIT_COUNT);
    if(load_st != SubGhzProtocolStatusOk) {
        return load_st;
    }

    uint32_t serial = (uint32_t)(instance->generic.data >> 32);
    uint32_t btn = (uint32_t)((instance->generic.data >> 24) & 0xFF);
    uint32_t cnt = (uint32_t)((instance->generic.data >> 8) & 0xFFFF);
    pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

    // Honor an overridden Cnt (Rolling-Code Analyzer Next/Burst overrides ONLY the
    // Cnt field): rebuild the frame for the exact requested counter so build_upload
    // emits it verbatim. Mirrors mazda_v0's encoder; build_upload no longer adds +1.
    instance->generic.data = ((uint64_t)serial << 32) | ((uint64_t)(btn & 0xFF) << 24) |
                             ((uint64_t)(cnt & 0xFFFF) << 8);

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 3);
    if(!mzs_build_upload(instance, (uint8_t)btn)) {
        return SubGhzProtocolStatusError;
    }

    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    pp_u64_to_bytes_be(instance->generic.data, key_data);
    if(!flipper_format_update_hex(flipper_format, FF_KEY, key_data, sizeof(key_data))) {
        return SubGhzProtocolStatusErrorParserKey;
    }

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_mazda_siemens_decoder = {
    .alloc = subghz_protocol_decoder_mazda_siemens_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_mazda_siemens_feed,
    .reset = subghz_protocol_decoder_mazda_siemens_reset,
    .get_hash_data = subghz_protocol_decoder_mazda_siemens_get_hash_data,
    .serialize = subghz_protocol_decoder_mazda_siemens_serialize,
    .deserialize = subghz_protocol_decoder_mazda_siemens_deserialize,
    .get_string = subghz_protocol_decoder_mazda_siemens_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_mazda_siemens_encoder = {
    .alloc = subghz_protocol_encoder_mazda_siemens_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_mazda_siemens_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_mazda_siemens_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol mazda_siemens_protocol = {
    .name = MAZDA_SIEMENS_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mazda_siemens_decoder,
    .encoder = &subghz_protocol_mazda_siemens_encoder,
};
