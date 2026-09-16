#include "sheriff_cfm.h"
#include "protocols_common.h"
#include "keeloq_common.h"

// Ported from KAT src/protocols/sheriff_cfm.rs
#define CFM_TE_SHORT  400
#define CFM_TE_LONG   800
#define CFM_TE_DELTA  140
#define CFM_BIT_COUNT 64

#define CFM_MODEL_ZX750 0
#define CFM_MODEL_ZX930 1

static const uint8_t CFM_PI_BYTES[16] = {
    0xA4, 0x58, 0xFE, 0xA3, 0xF4, 0x93, 0x3D, 0x7E,
    0x0D, 0x95, 0x74, 0x8F, 0x72, 0x8E, 0xB6, 0x58};
static const uint8_t CFM_ZX750_ENCODED[8] = {0x32, 0x4D, 0xCB, 0x84, 0x5F, 0xE9, 0x27, 0xCB};
static const uint8_t CFM_ZX930_ENCODED[8] = {0x94, 0x3B, 0x63, 0xA5, 0xE8, 0xF3, 0xAB, 0x60};

static const SubGhzBlockConst subghz_protocol_sheriff_cfm_const = {
    .te_short = CFM_TE_SHORT,
    .te_long = CFM_TE_LONG,
    .te_delta = CFM_TE_DELTA,
    .min_count_bit_for_found = CFM_BIT_COUNT,
};

#ifdef ENABLE_EMULATE_FEATURE
// 11 preamble pairs*2 + 2 + 64*2 + 2
#define CFM_UPLOAD_CAPACITY ((11U * 2U) + 2U + (CFM_BIT_COUNT * 2U) + 2U)
_Static_assert(
    CFM_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "CFM_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

typedef enum {
    CfmDecoderStepReset = 0,
    CfmDecoderStepCheckPreamble,
    CfmDecoderStepSaveDuration,
    CfmDecoderStepCheckDuration,
} CfmDecoderStep;

struct SubGhzProtocolDecoderSheriffCfm {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    uint64_t decode_data;
    uint16_t decode_count_bit;
    uint32_t te_last;
    uint8_t model;
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderSheriffCfm {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t model;
};
#endif

static uint8_t cfm_rlf(uint8_t v) {
    return (uint8_t)((v << 1) | (v >> 7));
}
static uint8_t cfm_rrf(uint8_t v) {
    return (uint8_t)((v >> 1) | (v << 7));
}
static uint8_t cfm_swap(uint8_t v) {
    return (uint8_t)((v >> 4) | (v << 4));
}

static void cfm_pi_decode(const uint8_t* encoded, size_t pi_offset, uint8_t* out) {
    for(int i = 0; i < 8; i++) out[i] = (uint8_t)(encoded[i] ^ CFM_PI_BYTES[pi_offset + i]);
}

static void cfm_decrypt_transform(uint8_t* hop, uint8_t model) {
    if(model == CFM_MODEL_ZX750) {
        hop[0] = cfm_swap(hop[0]);
        hop[2] = cfm_swap(hop[2]);
    } else {
        hop[0] = (uint8_t)(~hop[0]);
        uint8_t t = hop[1];
        hop[1] = hop[2];
        hop[2] = t;
        hop[0] = cfm_rrf(hop[0]);
        hop[1] = cfm_swap(hop[1]);
        hop[1] = cfm_rlf(hop[1]);
        hop[1] = cfm_rlf(hop[1]);
    }
}

// Inverse of cfm_decrypt_transform. Kept outside ENABLE_EMULATE_FEATURE so the
// next-code predictor (and decode-only builds) can rebuild a byte-correct frame.
static void cfm_encrypt_transform(uint8_t* hop, uint8_t model) {
    if(model == CFM_MODEL_ZX750) {
        hop[0] = cfm_swap(hop[0]);
        hop[2] = cfm_swap(hop[2]);
    } else {
        hop[1] = cfm_rrf(hop[1]);
        hop[1] = cfm_rrf(hop[1]);
        hop[1] = cfm_swap(hop[1]);
        hop[0] = cfm_rlf(hop[0]);
        hop[0] = (uint8_t)(~hop[0]);
        uint8_t t = hop[1];
        hop[1] = hop[2];
        hop[2] = t;
    }
}

static uint64_t cfm_get_mfkey(uint8_t model) {
    uint8_t dkey[8];
    if(model == CFM_MODEL_ZX750) {
        cfm_pi_decode(CFM_ZX750_ENCODED, 0, dkey);
    } else {
        cfm_pi_decode(CFM_ZX930_ENCODED, 8, dkey);
    }
    uint64_t key = 0;
    for(int i = 7; i >= 0; i--) key = (key << 8) | dkey[i];
    return key;
}

#define CFM_PREDICT_COUNT 3

// Next-code predictor. Drives the SAME encode chain as cfm_build_upload
// (hop_plain -> KeeLoq encrypt with the firmware-embedded mfkey -> per-model
// cfm_encrypt_transform -> (fix<<32)|hop) but for an ABSOLUTE counter value, so
// the returned 64-bit frame is byte-correct and decodes back to
// (serial, btn, cnt_abs). Not gated: the counter is cleartext and the mfkey is
// embedded, so prediction needs no secret and works in decode-only builds.
static uint64_t cfm_predict_key(uint8_t model, uint32_t serial, uint8_t btn, uint16_t cnt_abs) {
    uint16_t serial_lo = (uint16_t)(serial & 0x3FF);
    uint32_t hop_plain =
        ((uint32_t)btn << 24) | ((uint32_t)serial_lo << 16) | (uint32_t)cnt_abs;

    uint64_t mfkey = cfm_get_mfkey(model);
    uint32_t hop_encrypted = subghz_protocol_keeloq_common_encrypt(hop_plain, mfkey);

    uint8_t hop_bytes[4] = {
        (uint8_t)(hop_encrypted & 0xFF),
        (uint8_t)((hop_encrypted >> 8) & 0xFF),
        (uint8_t)((hop_encrypted >> 16) & 0xFF),
        (uint8_t)((hop_encrypted >> 24) & 0xFF)};
    cfm_encrypt_transform(hop_bytes, model);

    uint32_t hop_transformed = (uint32_t)hop_bytes[0] | ((uint32_t)hop_bytes[1] << 8) |
                               ((uint32_t)hop_bytes[2] << 16) | ((uint32_t)hop_bytes[3] << 24);

    return ((uint64_t)serial << 32) | (uint64_t)hop_transformed;
}

// Returns true on success and fills out params.
static bool cfm_try_decrypt(
    uint64_t data,
    uint8_t* out_model,
    uint8_t* out_btn,
    uint32_t* out_serial,
    uint16_t* out_cnt) {
    uint32_t hop_encrypted = (uint32_t)(data & 0xFFFFFFFF);
    uint32_t fix = (uint32_t)(data >> 32);

    for(uint8_t model = 0; model <= 1; model++) {
        uint8_t hop_bytes[4] = {
            (uint8_t)(hop_encrypted & 0xFF),
            (uint8_t)((hop_encrypted >> 8) & 0xFF),
            (uint8_t)((hop_encrypted >> 16) & 0xFF),
            (uint8_t)((hop_encrypted >> 24) & 0xFF)};

        cfm_decrypt_transform(hop_bytes, model);

        uint32_t hop_transformed = (uint32_t)hop_bytes[0] | ((uint32_t)hop_bytes[1] << 8) |
                                   ((uint32_t)hop_bytes[2] << 16) | ((uint32_t)hop_bytes[3] << 24);

        uint64_t mfkey = cfm_get_mfkey(model);
        uint32_t decrypted = subghz_protocol_keeloq_common_decrypt(hop_transformed, mfkey);

        uint16_t dec_serial_lo = (uint16_t)((decrypted >> 16) & 0x3FF);
        uint16_t fix_serial_lo = (uint16_t)(fix & 0x3FF);
        uint8_t btn_byte = (uint8_t)((decrypted >> 24) & 0xFF);
        bool valid_btn =
            (btn_byte == 0x10 || btn_byte == 0x20 || btn_byte == 0x40 || btn_byte == 0x80);

        if(valid_btn && dec_serial_lo == fix_serial_lo) {
            *out_model = model;
            *out_btn = btn_byte;
            *out_serial = fix;
            *out_cnt = (uint16_t)(decrypted & 0xFFFF);
            return true;
        }
    }
    return false;
}

static const char* cfm_model_name(uint8_t model) {
    return (model == CFM_MODEL_ZX930) ? "ZX-930" : "ZX-750";
}

void* subghz_protocol_decoder_sheriff_cfm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSheriffCfm* instance = calloc(1, sizeof(SubGhzProtocolDecoderSheriffCfm));
    furi_check(instance);
    instance->base.protocol = &sheriff_cfm_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_sheriff_cfm_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    instance->decoder.parser_step = CfmDecoderStepReset;
    instance->header_count = 0;
    instance->decode_data = 0;
    instance->decode_count_bit = 0;
    instance->te_last = 0;
    instance->model = CFM_MODEL_ZX750;
}

void subghz_protocol_decoder_sheriff_cfm_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;

    const uint32_t te_short = CFM_TE_SHORT;
    const uint32_t te_long = CFM_TE_LONG;
    const uint32_t te_delta = CFM_TE_DELTA;

    switch(instance->decoder.parser_step) {
    case CfmDecoderStepReset:
        if(level && DURATION_DIFF(duration, te_short) < te_delta) {
            instance->decoder.parser_step = CfmDecoderStepCheckPreamble;
            instance->header_count++;
        }
        break;

    case CfmDecoderStepCheckPreamble:
        if(!level && DURATION_DIFF(duration, te_short) < te_delta) {
            instance->decoder.parser_step = CfmDecoderStepReset;
        } else if(
            instance->header_count > 2 &&
            DURATION_DIFF(duration, te_short * 10) < te_delta * 10) {
            instance->decoder.parser_step = CfmDecoderStepSaveDuration;
            instance->decode_data = 0;
            instance->decode_count_bit = 0;
        } else {
            instance->decoder.parser_step = CfmDecoderStepReset;
            instance->header_count = 0;
        }
        break;

    case CfmDecoderStepSaveDuration:
        if(level) {
            instance->te_last = duration;
            instance->decoder.parser_step = CfmDecoderStepCheckDuration;
        }
        break;

    case CfmDecoderStepCheckDuration:
        if(!level) {
            if(duration >= (te_short * 2 + te_delta)) {
                instance->decoder.parser_step = CfmDecoderStepReset;
                if(instance->decode_count_bit >= CFM_BIT_COUNT &&
                   instance->decode_count_bit <= CFM_BIT_COUNT + 2) {
                    uint8_t model, btn;
                    uint32_t serial;
                    uint16_t cnt;
                    if(cfm_try_decrypt(instance->decode_data, &model, &btn, &serial, &cnt)) {
                        instance->model = model;
                        instance->generic.data = instance->decode_data;
                        instance->generic.data_count_bit = CFM_BIT_COUNT;
                        instance->generic.serial = serial;
                        instance->generic.btn = btn;
                        instance->generic.cnt = cnt;
                        if(instance->base.callback) {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decode_data = 0;
                instance->decode_count_bit = 0;
                instance->header_count = 0;
            } else if(
                DURATION_DIFF(instance->te_last, te_short) < te_delta &&
                DURATION_DIFF(duration, te_long) < te_delta * 2) {
                if(instance->decode_count_bit < CFM_BIT_COUNT) {
                    instance->decode_data = (instance->decode_data << 1) | 1U;
                }
                instance->decode_count_bit++;
                instance->decoder.parser_step = CfmDecoderStepSaveDuration;
            } else if(
                DURATION_DIFF(instance->te_last, te_long) < te_delta * 2 &&
                DURATION_DIFF(duration, te_short) < te_delta) {
                if(instance->decode_count_bit < CFM_BIT_COUNT) {
                    instance->decode_data = (instance->decode_data << 1);
                }
                instance->decode_count_bit++;
                instance->decoder.parser_step = CfmDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = CfmDecoderStepReset;
                instance->header_count = 0;
            }
        } else {
            instance->decoder.parser_step = CfmDecoderStepReset;
            instance->header_count = 0;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_sheriff_cfm_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_sheriff_cfm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
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
        uint32_t model = instance->model;
        flipper_format_write_uint32(flipper_format, "Model", &model, 1);
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_sheriff_cfm_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_sheriff_cfm_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        uint8_t model, btn;
        uint32_t serial;
        uint16_t cnt;
        if(cfm_try_decrypt(instance->generic.data, &model, &btn, &serial, &cnt)) {
            instance->model = model;
            instance->generic.serial = serial;
            instance->generic.btn = btn;
            instance->generic.cnt = cnt;
        }
    }
    return ret;
}

void subghz_protocol_decoder_sheriff_cfm_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderSheriffCfm* instance = context;
    furi_string_cat_printf(
        output,
        "%s (%s)\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%08lX Btn:%02X\r\n"
        "Cnt:%04X\r\n",
        instance->generic.protocol_name,
        cfm_model_name(instance->model),
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF),
        (unsigned long)instance->generic.serial,
        (unsigned int)instance->generic.btn,
        (unsigned int)instance->generic.cnt);

    // Predicted upcoming rolling codes. btn is only ever the decode whitelist
    // (0x10/0x20/0x40/0x80) after a valid frame; 0 on a fresh instance -> skip.
    if(instance->generic.btn != 0) {
        for(uint8_t i = 1; i <= CFM_PREDICT_COUNT; i++) {
            uint16_t next_cnt = (uint16_t)(instance->generic.cnt + i);
            uint64_t next_key = cfm_predict_key(
                instance->model, instance->generic.serial, instance->generic.btn, next_cnt);
            furi_string_cat_printf(
                output,
                "\r\nNext+%u:%08lX%08lX",
                (unsigned int)i,
                (unsigned long)(next_key >> 32),
                (unsigned long)(next_key & 0xFFFFFFFF));
        }
    }
}

#ifdef ENABLE_EMULATE_FEATURE
static bool cfm_build_upload(SubGhzProtocolEncoderSheriffCfm* instance, uint8_t button) {
    uint32_t fix = instance->generic.serial;
    uint16_t cnt = (uint16_t)(instance->generic.cnt + 1);
    uint16_t serial_lo = (uint16_t)(fix & 0x3FF);
    uint32_t hop_plain =
        ((uint32_t)button << 24) | ((uint32_t)serial_lo << 16) | (uint32_t)cnt;

    uint64_t mfkey = cfm_get_mfkey(instance->model);
    uint32_t hop_encrypted = subghz_protocol_keeloq_common_encrypt(hop_plain, mfkey);

    uint8_t hop_bytes[4] = {
        (uint8_t)(hop_encrypted & 0xFF),
        (uint8_t)((hop_encrypted >> 8) & 0xFF),
        (uint8_t)((hop_encrypted >> 16) & 0xFF),
        (uint8_t)((hop_encrypted >> 24) & 0xFF)};
    cfm_encrypt_transform(hop_bytes, instance->model);

    uint32_t hop_transformed = (uint32_t)hop_bytes[0] | ((uint32_t)hop_bytes[1] << 8) |
                               ((uint32_t)hop_bytes[2] << 16) | ((uint32_t)hop_bytes[3] << 24);

    uint64_t data = ((uint64_t)fix << 32) | (uint64_t)hop_transformed;

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = CFM_UPLOAD_CAPACITY;
    size_t index = 0;

    for(int p = 0; p < 11; p++) {
        index = pp_emit(up, index, cap, true, CFM_TE_SHORT);
        index = pp_emit(up, index, cap, false, CFM_TE_SHORT);
    }
    index = pp_emit(up, index, cap, true, CFM_TE_SHORT);
    index = pp_emit(up, index, cap, false, CFM_TE_SHORT * 10);

    for(int8_t i = CFM_BIT_COUNT - 1; i >= 0; i--) {
        if((data >> i) & 1U) {
            index = pp_emit(up, index, cap, true, CFM_TE_SHORT);
            index = pp_emit(up, index, cap, false, CFM_TE_LONG);
        } else {
            index = pp_emit(up, index, cap, true, CFM_TE_LONG);
            index = pp_emit(up, index, cap, false, CFM_TE_SHORT);
        }
    }
    index = pp_emit(up, index, cap, true, CFM_TE_SHORT);
    index = pp_emit(up, index, cap, false, CFM_TE_SHORT * 40);

    instance->generic.data = data;
    instance->generic.cnt = cnt;

    if(index > cap) return false;
    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_sheriff_cfm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSheriffCfm* instance = calloc(1, sizeof(SubGhzProtocolEncoderSheriffCfm));
    furi_check(instance);
    instance->base.protocol = &sheriff_cfm_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 5;
    instance->model = CFM_MODEL_ZX750;
    pp_encoder_buffer_ensure(instance, CFM_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_sheriff_cfm_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderSheriffCfm* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }
    SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, CFM_BIT_COUNT);
    if(load_st != SubGhzProtocolStatusOk) {
        return load_st;
    }

    // Recover fields (serial/btn/cnt/model) from the saved frame + fields.
    uint8_t model = CFM_MODEL_ZX750, btn = 0x10;
    uint32_t serial = 0;
    uint16_t cnt = 0;
    if(cfm_try_decrypt(instance->generic.data, &model, &btn, &serial, &cnt)) {
        instance->model = model;
        instance->generic.serial = serial;
        instance->generic.btn = btn;
        instance->generic.cnt = cnt;
    }
    uint32_t model_field = instance->model;
    if(flipper_format_read_uint32(flipper_format, "Model", &model_field, 1)) {
        instance->model = (uint8_t)(model_field & 1);
    }

    uint32_t s = instance->generic.serial;
    uint32_t b = instance->generic.btn ? instance->generic.btn : 0x10;
    uint32_t c = instance->generic.cnt;
    pp_encoder_read_fields(flipper_format, &s, &b, &c, NULL);
    instance->generic.serial = s;
    instance->generic.btn = (uint8_t)b;
    instance->generic.cnt = (uint16_t)c;

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 5);
    if(!cfm_build_upload(instance, instance->generic.btn ? instance->generic.btn : 0x10)) {
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

const SubGhzProtocolDecoder subghz_protocol_sheriff_cfm_decoder = {
    .alloc = subghz_protocol_decoder_sheriff_cfm_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_sheriff_cfm_feed,
    .reset = subghz_protocol_decoder_sheriff_cfm_reset,
    .get_hash_data = subghz_protocol_decoder_sheriff_cfm_get_hash_data,
    .serialize = subghz_protocol_decoder_sheriff_cfm_serialize,
    .deserialize = subghz_protocol_decoder_sheriff_cfm_deserialize,
    .get_string = subghz_protocol_decoder_sheriff_cfm_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_sheriff_cfm_encoder = {
    .alloc = subghz_protocol_encoder_sheriff_cfm_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_sheriff_cfm_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_sheriff_cfm_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol sheriff_cfm_protocol = {
    .name = SHERIFF_CFM_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_sheriff_cfm_decoder,
    .encoder = &subghz_protocol_sheriff_cfm_encoder,
};
