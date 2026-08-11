#include "porsche_touareg.h"
#include "protocols_common.h"
#include <string.h>

// Original implementation by @lupettohf

#define PORSCHE_CAYENNE_BIT_COUNT 64
#define PC_TE_SYNC                3370U
#define PC_TE_GAP                 5930U
#define PC_SYNC_MIN               15

static const SubGhzBlockConst subghz_protocol_porsche_cayenne_const = {
    .te_short = 1680,
    .te_long = 3370,
    .te_delta = 500,
    .min_count_bit_for_found = PORSCHE_CAYENNE_BIT_COUNT,
};

typedef enum {
    PorscheCayenneDecoderStepReset = 0,
    PorscheCayenneDecoderStepSync,
    PorscheCayenneDecoderStepGapHigh,
    PorscheCayenneDecoderStepGapLow,
    PorscheCayenneDecoderStepData,
} PorscheCayenneDecoderStep;

struct SubGhzProtocolDecoderPorscheCayenne {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t sync_count;
    uint64_t raw_data;
    uint8_t bit_count;
};

static void porsche_cayenne_compute_frame(
    uint32_t serial24,
    uint8_t btn,
    uint16_t counter,
    uint8_t frame_type,
    uint8_t* pkt) {
    uint8_t b0 = (uint8_t)((btn << 4) | (frame_type & 0x07));
    uint8_t b1 = (serial24 >> 16) & 0xFF;
    uint8_t b2 = (serial24 >> 8) & 0xFF;
    uint8_t b3 = serial24 & 0xFF;

    uint16_t cnt = counter + 1;
    uint8_t cnt_lo = cnt & 0xFF;
    uint8_t cnt_hi = (cnt >> 8) & 0xFF;

    uint8_t r_h = b3;
    uint8_t r_m = b1;
    uint8_t r_l = b2;

#define ROTATE24(rh, rm, rl)                 \
    do {                                     \
        uint8_t _ch = ((rh) >> 7) & 1U;      \
        uint8_t _cm = ((rm) >> 7) & 1U;      \
        uint8_t _cl = ((rl) >> 7) & 1U;      \
        (rh) = (uint8_t)(((rh) << 1) | _cm); \
        (rm) = (uint8_t)(((rm) << 1) | _cl); \
        (rl) = (uint8_t)(((rl) << 1) | _ch); \
    } while(0)

    for(uint8_t i = 0; i < 4; i++) {
        ROTATE24(r_h, r_m, r_l);
    }
    for(uint16_t i = 0; i < cnt_lo; i++) {
        ROTATE24(r_h, r_m, r_l);
    }

#undef ROTATE24

    uint8_t a9a = r_h ^ b0;

    uint8_t nb9b_p1 = (uint8_t)((~cnt_lo << 2) & 0xFC) ^ r_m;
    uint8_t nb9b_p2 = (uint8_t)((~cnt_hi << 2) & 0xFC) ^ r_m;
    uint8_t nb9b_p3 = (uint8_t)((~cnt_hi >> 6) & 0x03) ^ r_m;
    uint8_t a9b = (nb9b_p1 & 0xCC) | (nb9b_p2 & 0x30) | (nb9b_p3 & 0x03);

    uint8_t nb9c_p1 = (uint8_t)((~cnt_lo >> 2) & 0x3F) ^ r_l;
    uint8_t nb9c_p2 = (uint8_t)((~cnt_hi & 0x03) << 6) ^ r_l;
    uint8_t nb9c_p3 = (uint8_t)((~cnt_hi >> 2) & 0x3F) ^ r_l;
    uint8_t a9c = (nb9c_p1 & 0x33) | (nb9c_p2 & 0xC0) | (nb9c_p3 & 0x0C);

    pkt[0] = b0;
    pkt[1] = b1;
    pkt[2] = b2;
    pkt[3] = b3;
    pkt[4] = (uint8_t)(((a9a >> 2) & 0x3F) | ((~cnt_lo & 0x03U) << 6));
    pkt[5] = (uint8_t)((~cnt_lo & 0xC0U) | ((a9a & 0x03U) << 4) | (a9b & 0x0CU) |
                       ((~cnt_lo >> 2) & 0x03U));
    pkt[6] = (uint8_t)(((a9b & 0x03U) << 6) | ((a9c >> 2) & 0x3CU) | ((~cnt_lo >> 4) & 0x03U));
    pkt[7] = (uint8_t)(((a9b >> 4) & 0x0FU) | ((a9c & 0x0FU) << 4));
}

static void porsche_cayenne_parse_data(SubGhzProtocolDecoderPorscheCayenne* instance) {
    uint8_t pkt[8];
    uint64_t raw = instance->generic.data;

    for(int8_t i = 7; i >= 0; i--) {
        pkt[i] = (uint8_t)(raw & 0xFF);
        raw >>= 8;
    }

    instance->generic.serial = ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | pkt[3];
    instance->generic.btn = (uint8_t)(pkt[0] >> 4);
    instance->generic.cnt = 0;

    uint8_t frame_type = pkt[0] & 0x07;
    uint8_t try_pkt[8];
    for(uint16_t try_cnt = 1; try_cnt <= 256; try_cnt++) {
        porsche_cayenne_compute_frame(
            instance->generic.serial,
            instance->generic.btn,
            (uint16_t)(try_cnt - 1),
            frame_type,
            try_pkt);
        if(try_pkt[4] == pkt[4] && try_pkt[5] == pkt[5] && try_pkt[6] == pkt[6] &&
           try_pkt[7] == pkt[7]) {
            instance->generic.cnt = try_cnt;
            break;
        }
    }
}

static void porsche_cayenne_publish_frame(SubGhzProtocolDecoderPorscheCayenne* instance) {
    instance->generic.data = instance->raw_data;
    instance->generic.data_count_bit = PORSCHE_CAYENNE_BIT_COUNT;
    porsche_cayenne_parse_data(instance);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

const SubGhzProtocolDecoder subghz_protocol_porsche_cayenne_decoder = {
    .alloc = subghz_protocol_decoder_porsche_cayenne_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_porsche_cayenne_feed,
    .reset = subghz_protocol_decoder_porsche_cayenne_reset,
    .get_hash_data = subghz_protocol_decoder_porsche_cayenne_get_hash_data,
    .serialize = subghz_protocol_decoder_porsche_cayenne_serialize,
    .deserialize = subghz_protocol_decoder_porsche_cayenne_deserialize,
    .get_string = subghz_protocol_decoder_porsche_cayenne_get_string,
};

#if PROTOPIRATE_WITH_ENCODER

// TX is a real rolling encoder: porsche_cayenne_compute_frame() above is the
// full, key-less frame generator (serial + button + counter -> 8 bytes), so a
// fresh valid frame is produced for each counter. Frame type defaults to
// "First" (0x02) since captures don't persist it.
#define PC_TE_SHORT          1680U
#define PC_TE_LONG           3370U
#define PC_TX_SYNC_US        PC_TE_SYNC
#define PC_TX_GAP_US         PC_TE_GAP
#define PC_TX_SYNC_PULSES    16U
#define PC_TX_DEFAULT_FTYPE  0x02U
#define PORSCHE_CAYENNE_UPLOAD_CAPACITY \
    ((PC_TX_SYNC_PULSES * 2U) + 4U + (PORSCHE_CAYENNE_BIT_COUNT * 2U) + 2U)
_Static_assert(
    PORSCHE_CAYENNE_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "PORSCHE_CAYENNE_UPLOAD_CAPACITY exceeds shared upload slab");

typedef struct SubGhzProtocolEncoderPorscheCayenne {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t frame_type;
} SubGhzProtocolEncoderPorscheCayenne;

static bool porsche_cayenne_build_upload(SubGhzProtocolEncoderPorscheCayenne* instance) {
    furi_check(instance);

    uint8_t pkt[8];
    porsche_cayenne_compute_frame(
        instance->generic.serial & 0xFFFFFFU,
        (uint8_t)instance->generic.btn,
        (uint16_t)instance->generic.cnt,
        instance->frame_type ? instance->frame_type : PC_TX_DEFAULT_FTYPE,
        pkt);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = PORSCHE_CAYENNE_UPLOAD_CAPACITY;
    size_t i = 0;

    // Sync burst of ~3370us pulses, then a wide gap into the data.
    for(size_t s = 0; s < PC_TX_SYNC_PULSES; s++) {
        i = pp_emit(up, i, cap, true, PC_TX_SYNC_US);
        i = pp_emit(up, i, cap, false, PC_TX_SYNC_US);
    }
    i = pp_emit(up, i, cap, true, PC_TX_GAP_US);
    i = pp_emit(up, i, cap, false, PC_TX_GAP_US);

    // Data bits: decoder reads each bit as (low_dur, high_dur):
    // (short,long) => 0, (long,short) => 1.
    for(uint8_t b = 0; b < PORSCHE_CAYENNE_BIT_COUNT; b++) {
        const bool bit = (pkt[b >> 3] >> (7U - (b & 7U))) & 1U;
        if(bit) {
            i = pp_emit(up, i, cap, false, PC_TE_LONG);
            i = pp_emit(up, i, cap, true, PC_TE_SHORT);
        } else {
            i = pp_emit(up, i, cap, false, PC_TE_SHORT);
            i = pp_emit(up, i, cap, true, PC_TE_LONG);
        }
    }
    i = pp_emit(up, i, cap, false, PC_TX_GAP_US);

    instance->encoder.front = 0;
    instance->encoder.size_upload = i;
    return i > 0;
}

void* subghz_protocol_encoder_porsche_cayenne_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPorscheCayenne* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderPorscheCayenne));
    furi_check(instance);
    instance->base.protocol = &porsche_touareg_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 6;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    instance->frame_type = PC_TX_DEFAULT_FTYPE;
    pp_encoder_buffer_ensure(instance, PORSCHE_CAYENNE_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderPorscheCayenne* instance = context;
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
               subghz_protocol_porsche_cayenne_const.min_count_bit_for_found) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        // Restore the persisted frame type; captures saved before the Type field
        // existed fall back to First (PC_TX_DEFAULT_FTYPE).
        uint32_t frame_type = PC_TX_DEFAULT_FTYPE;
        pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, &frame_type);
        instance->generic.serial = serial & 0xFFFFFFU;
        instance->generic.btn = (uint8_t)btn;
        instance->generic.cnt = cnt;
        instance->frame_type = (uint8_t)(frame_type & 0x07U);

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 6);

        if(!porsche_cayenne_build_upload(instance) || instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_porsche_cayenne_encoder = {
    .alloc = subghz_protocol_encoder_porsche_cayenne_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_porsche_cayenne_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_porsche_cayenne_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol porsche_touareg_protocol = {
    .name = PORSCHE_CAYENNE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save
#if PROTOPIRATE_WITH_ENCODER
            | SubGhzProtocolFlag_Send
#endif
    ,
#if PROTOPIRATE_WITH_DECODER
    .decoder = &subghz_protocol_porsche_cayenne_decoder,
#else
    .decoder = NULL,
#endif
#if PROTOPIRATE_WITH_ENCODER
    .encoder = &subghz_protocol_porsche_cayenne_encoder,
#else
    .encoder = NULL,
#endif
};

void* subghz_protocol_decoder_porsche_cayenne_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPorscheCayenne* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderPorscheCayenne));
    furi_check(instance);
    instance->base.protocol = &porsche_touareg_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_porsche_cayenne_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;
    instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->sync_count = 0;
    instance->raw_data = 0;
    instance->bit_count = 0;
    instance->generic.data = 0;
    instance->generic.data_count_bit = 0;
}

void subghz_protocol_decoder_porsche_cayenne_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    const uint32_t te_short = subghz_protocol_porsche_cayenne_const.te_short;
    const uint32_t te_long = subghz_protocol_porsche_cayenne_const.te_long;
    const uint32_t te_delta = subghz_protocol_porsche_cayenne_const.te_delta;

    switch(instance->decoder.parser_step) {
    case PorscheCayenneDecoderStepReset:
        if(!level && DURATION_DIFF(duration, PC_TE_SYNC) < te_delta) {
            instance->sync_count = 1;
            instance->decoder.parser_step = PorscheCayenneDecoderStepSync;
        }
        break;

    case PorscheCayenneDecoderStepSync:
        if(level) {
            if(DURATION_DIFF(duration, PC_TE_SYNC) < te_delta) {
                // keep collecting sync pairs
            } else if(
                instance->sync_count >= PC_SYNC_MIN &&
                DURATION_DIFF(duration, PC_TE_GAP) < te_delta) {
                instance->decoder.parser_step = PorscheCayenneDecoderStepGapLow;
            } else {
                instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
            }
        } else {
            if(DURATION_DIFF(duration, PC_TE_SYNC) < te_delta) {
                instance->sync_count++;
            } else if(
                instance->sync_count >= PC_SYNC_MIN &&
                DURATION_DIFF(duration, PC_TE_GAP) < te_delta) {
                instance->decoder.parser_step = PorscheCayenneDecoderStepGapHigh;
            } else {
                instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
            }
        }
        break;

    case PorscheCayenneDecoderStepGapHigh:
        if(level && DURATION_DIFF(duration, PC_TE_GAP) < te_delta) {
            instance->raw_data = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = PorscheCayenneDecoderStepData;
        } else {
            instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
        }
        break;

    case PorscheCayenneDecoderStepGapLow:
        if(!level && DURATION_DIFF(duration, PC_TE_GAP) < te_delta) {
            instance->raw_data = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = PorscheCayenneDecoderStepData;
        } else {
            instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
        }
        break;

    case PorscheCayenneDecoderStepData:
        if(level) {
            bool bit_value = false;
            if(DURATION_DIFF(instance->decoder.te_last, te_short) < te_delta &&
               DURATION_DIFF(duration, te_long) < te_delta) {
                bit_value = false;
            } else if(
                DURATION_DIFF(instance->decoder.te_last, te_long) < te_delta &&
                DURATION_DIFF(duration, te_short) < te_delta) {
                bit_value = true;
            } else {
                instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
                break;
            }

            instance->raw_data = (instance->raw_data << 1) | (bit_value ? 1U : 0U);
            instance->bit_count++;

            if(instance->bit_count >= PORSCHE_CAYENNE_BIT_COUNT) {
                porsche_cayenne_publish_frame(instance);
                instance->decoder.parser_step = PorscheCayenneDecoderStepReset;
            }
        } else {
            instance->decoder.te_last = duration;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_porsche_cayenne_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        // Persist the decoded frame type (First/Cont/Final = bits 58..56 of the
        // frame). frame_type is XORed into the obfuscation, so a captured
        // Cont/Final frame must not be re-encoded as the default First.
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN | PP_FIELD_CNT | PP_FIELD_TYPE,
            instance->generic.serial & 0xFFFFFF,
            instance->generic.btn,
            instance->generic.cnt,
            (uint8_t)((instance->generic.data >> 56) & 0x07));
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_porsche_cayenne_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_porsche_cayenne_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        porsche_cayenne_parse_data(instance);

        uint32_t serial = 0;
        if(flipper_format_read_uint32(flipper_format, FF_SERIAL, &serial, 1)) {
            instance->generic.serial = serial & 0xFFFFFF;
        }

        uint32_t cnt = 0;
        if(flipper_format_read_uint32(flipper_format, FF_CNT, &cnt, 1)) {
            instance->generic.cnt = cnt;
        }

        uint32_t btn = 0;
        if(flipper_format_read_uint32(flipper_format, FF_BTN, &btn, 1)) {
            instance->generic.btn = (uint8_t)btn;
        }
    }

    return ret;
}

void subghz_protocol_decoder_porsche_cayenne_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderPorscheCayenne* instance = context;

    uint8_t frame_type = (uint8_t)((instance->generic.data >> 56) & 0x07);
    const char* frame_type_name = "??";
    if(frame_type == 0x02) {
        frame_type_name = "First";
    } else if(frame_type == 0x01) {
        frame_type_name = "Cont";
    } else if(frame_type == 0x04) {
        frame_type_name = "Final";
    }

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%06lX Btn:%X\r\n"
        "Cnt:%04lX FT:%s\r\n"
        "Raw:%08lX%08lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.serial & 0xFFFFFF),
        (unsigned int)instance->generic.btn,
        (unsigned long)instance->generic.cnt,
        frame_type_name,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
}
