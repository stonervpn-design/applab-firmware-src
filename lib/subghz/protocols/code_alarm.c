#include "code_alarm.h"
#include "protocols_common.h"
#include "../defines.h" /* ENABLE_EMULATE_FEATURE */
#include <lib/toolbox/manchester_decoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Code Alarm FRDPC2002 / GOH-FRDPC2002 Car Remote
 *
 * Ported from the rtl_433 decoder src/devices/code_alarm_car_remote.c
 * (Copyright (C) 2024 Ethan Halsall, GPLv2+).
 *
 * RF profile:
 *   - Carrier ~433.92 MHz (some units 315 MHz), OOK
 *   - Modulation: Manchester (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT)
 *       half-bit (TE short) ~550us, full-bit (TE long) ~1100us
 *   - Reset / inter-frame gap ~1600us delimits repeated copies
 *   - Frame length: exactly 60 Manchester bits, one row, repeated while held
 *   - Preamble: 16 leading zero bits (0x0000)
 *
 * Frame layout (60 bits, MSB-first, GET(s,l)=(data>>(60-s-l))&mask):
 *   bits  0..15 = preamble (must be 0x0000)
 *   bits 16..18 = unknown (ignored)
 *   bits 19..58 = 40-bit body -> body[0..4]
 *   bits 23..58 = 36-bit rolling code -> code[0..3] (+4 unused bits)
 *   bit  59     = trailing unknown (ignored)
 *
 * Fields:
 *   id (24-bit, XOR-folded): ((code0^code1)<<16)|((code1^code2)<<8)|(code2^code3)
 *   button (4-bit value): body[0] >> 4
 *   counter: 36-bit rolling code present but uncracked -> cnt = 0
 *
 * rtl_433 has NO CRC on this remote; validation is three sanity checks:
 *   1) exactly 60 bits collected
 *   2) preamble GET(0,16) == 0
 *   3) sum(body[0..4]) != 0 and < 0xFF*5 (rejects all-zero / all-0xFF bodies)
 *
 * Receive/decode-only (rolling code not cracked). Encoder is all-NULL.
 */

#define TAG "CodeAlarm"

#define CODE_ALARM_TE_SHORT 550
#define CODE_ALARM_TE_LONG  1100
#define CODE_ALARM_TE_DELTA 200

#define CODE_ALARM_BIT_COUNT 60
#define CODE_ALARM_MAX_BITS  72 /* runaway guard */

#define CODE_ALARM_DATA_MASK 0x0FFFFFFFFFFFFFFFULL /* low 60 bits */

/* Inter-frame reset gap that delimits repeated frame copies (see decoder). */
#define CODE_ALARM_TE_GAP 1600

#ifdef ENABLE_EMULATE_FEATURE
/* 60 Manchester bits * 2 half-bits + 1 inter-frame gap entry. */
#define CODE_ALARM_UPLOAD_CAPACITY ((CODE_ALARM_BIT_COUNT * 2U) + 2U)
_Static_assert(
    CODE_ALARM_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "CODE_ALARM_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

static const SubGhzBlockConst subghz_protocol_code_alarm_const = {
    .te_short = CODE_ALARM_TE_SHORT,
    .te_long = CODE_ALARM_TE_LONG,
    .te_delta = CODE_ALARM_TE_DELTA,
    .min_count_bit_for_found = CODE_ALARM_BIT_COUNT,
};

typedef enum {
    CodeAlarmDecoderStepReset = 0,
    CodeAlarmDecoderStepDecode,
} CodeAlarmDecoderStep;

struct SubGhzProtocolDecoderCodeAlarm {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
};

typedef struct SubGhzProtocolDecoderCodeAlarm SubGhzProtocolDecoderCodeAlarm;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_code_alarm_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_code_alarm_free(void* context);
static void subghz_protocol_decoder_code_alarm_reset(void* context);
static void subghz_protocol_decoder_code_alarm_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_code_alarm_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_code_alarm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    subghz_protocol_decoder_code_alarm_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_decoder_code_alarm_get_string(void* context, FuriString* output);

// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_code_alarm_decoder = {
    .alloc = subghz_protocol_decoder_code_alarm_alloc,
    .free = subghz_protocol_decoder_code_alarm_free,
    .feed = subghz_protocol_decoder_code_alarm_feed,
    .reset = subghz_protocol_decoder_code_alarm_reset,
    .get_hash_data = subghz_protocol_decoder_code_alarm_get_hash_data,
    .serialize = subghz_protocol_decoder_code_alarm_serialize,
    .deserialize = subghz_protocol_decoder_code_alarm_deserialize,
    .get_string = subghz_protocol_decoder_code_alarm_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
// The 36-bit rolling code is uncracked (encrypted/unknown transform), so we do
// NOT predict future codes. The whole 60-bit on-air frame is stored verbatim in
// generic.data, so a byte-correct ONE-SHOT verbatim replay of the captured frame
// is fully reconstructable. There is no CRC/parity to recompute (rtl_433 has
// none), so byte-correctness == the stored 60 bits re-emitted unchanged.
static void* subghz_protocol_encoder_code_alarm_alloc(SubGhzEnvironment* environment);
static SubGhzProtocolStatus
    subghz_protocol_encoder_code_alarm_deserialize(void* context, FlipperFormat* flipper_format);

const SubGhzProtocolEncoder subghz_protocol_code_alarm_encoder = {
    .alloc = subghz_protocol_encoder_code_alarm_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_code_alarm_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_code_alarm_encoder = {
    // Emulate disabled: no TX. NULL ops, no stub funcs.
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_code_alarm = {
    .name = CODE_ALARM_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_code_alarm_decoder,
    .encoder = &subghz_protocol_code_alarm_encoder,
};

// ============================================================================
// Encoder: verbatim one-shot replay (ENABLE_EMULATE_FEATURE only)
//
// Rebuilds the exact 60-bit Manchester frame the decoder captured. generic.data
// holds the 60 bits MSB-first in its low 60 bits (preamble included). The
// decoder does `data = !data` after manchester_advance (IEEE convention), so
// the on-air Manchester bit for stored bit s is b = !s. pp_emit_manchester_bit
// is the exact inverse of this tree's manchester_advance slicer (same pairing
// used by mazda_v0/ford_v0 etc.), so emitting b reproduces s at the receiver.
// A ~1600us idle gap follows each frame; the encoder repeats the whole upload,
// re-creating the "repeated while held" transmission.
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE
typedef struct SubGhzProtocolEncoderCodeAlarm {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderCodeAlarm;

static bool subghz_protocol_encoder_code_alarm_get_upload(SubGhzProtocolEncoderCodeAlarm* instance) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = CODE_ALARM_UPLOAD_CAPACITY;
    const uint32_t te = (uint32_t)subghz_protocol_code_alarm_const.te_short;
    const uint64_t data = instance->generic.data & CODE_ALARM_DATA_MASK;
    size_t index = 0;

    // 60 bits MSB-first, preamble included; on-air bit = !(stored bit).
    for(int8_t i = CODE_ALARM_BIT_COUNT - 1; i >= 0; i--) {
        bool bit_on_air = ((data >> i) & 1ULL) == 0ULL;
        index = pp_emit_manchester_bit(up, index, cap, bit_on_air, te);
    }

    // Inter-frame reset gap (idle low) delimiting each repeated copy.
    index = pp_emit(up, index, cap, false, CODE_ALARM_TE_GAP);

    if(index > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

static void* subghz_protocol_encoder_code_alarm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderCodeAlarm* instance = calloc(1, sizeof(SubGhzProtocolEncoderCodeAlarm));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_code_alarm;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, CODE_ALARM_UPLOAD_CAPACITY);

    return instance;
}

static SubGhzProtocolStatus
    subghz_protocol_encoder_code_alarm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderCodeAlarm* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_code_alarm_const.min_count_bit_for_found);
        if(load_st != SubGhzProtocolStatusOk) {
            ret = load_st;
            break;
        }

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

        // Verbatim replay: emit the stored 60-bit frame unchanged (no rolling
        // prediction — the 36-bit code is uncracked).
        if(!subghz_protocol_encoder_code_alarm_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
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
// Frame extraction helpers (shared by decode + display)
// ============================================================================

typedef struct {
    uint32_t id; /* 24-bit XOR-folded ID */
    uint8_t button; /* 4-bit button value (bits 19..22) */
    uint8_t body[5]; /* 40-bit body, bytes 0..4 (bits 19..58) */
} CodeAlarmFrame;

/*
 * Reconstruct/validate a 60-bit frame held MSB-first in the low 60 bits of
 * `data`. Returns true and fills `out` when all rtl_433 sanity checks pass.
 */
static bool subghz_protocol_code_alarm_parse(uint64_t data, CodeAlarmFrame* out) {
    /* (2) preamble: top 16 bits must be zero -> GET(0,16) == 0 */
    if(((data >> 44) & 0xFFFFULL) != 0) {
        return false;
    }

    /* 40-bit body extracted from bit 19 (MSB-first) */
    uint8_t body[5];
    body[0] = (uint8_t)((data >> 33) & 0xFF);
    body[1] = (uint8_t)((data >> 25) & 0xFF);
    body[2] = (uint8_t)((data >> 17) & 0xFF);
    body[3] = (uint8_t)((data >> 9) & 0xFF);
    body[4] = (uint8_t)((data >> 1) & 0xFF);

    /* (3) byte-sum sanity: reject all-zero and all-0xFF bodies */
    uint32_t sum = (uint32_t)body[0] + body[1] + body[2] + body[3] + body[4];
    if(sum == 0 || sum >= (uint32_t)(0xFF * 5)) {
        return false;
    }

    /* 36-bit rolling code extracted from bit 23 (MSB-first) */
    uint8_t code0 = (uint8_t)((data >> 29) & 0xFF);
    uint8_t code1 = (uint8_t)((data >> 21) & 0xFF);
    uint8_t code2 = (uint8_t)((data >> 13) & 0xFF);
    uint8_t code3 = (uint8_t)((data >> 5) & 0xFF);

    /* 24-bit XOR-folded ID */
    uint32_t id = ((uint32_t)(code0 ^ code1) << 16) | ((uint32_t)(code1 ^ code2) << 8) |
                  (uint32_t)(code2 ^ code3);

    out->id = id;
    out->button = (uint8_t)(body[0] >> 4);
    out->body[0] = body[0];
    out->body[1] = body[1];
    out->body[2] = body[2];
    out->body[3] = body[3];
    out->body[4] = body[4];
    return true;
}

/*
 * Append the rtl_433 button name(s) for a 4-bit button value. A value may map
 * to two names; they are joined with "; ". Falls back to "?" when unknown.
 */
static void subghz_protocol_code_alarm_button_name(uint8_t button, char* buf, size_t sz) {
    buf[0] = '\0';
    bool first = true;

    struct {
        bool match;
        const char* name;
    } table[] = {
        {button == 0x7, "Multiple"},
        {button == 0x6 || button == 0x4, "Lock"},
        {button == 0x1 || button == 0x3, "Panic"},
        {button == 0x0 || button == 0x3, "Start"},
        {button == 0x5 || button == 0x4, "Unlock"},
    };

    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if(table[i].match) {
            if(!first) strlcat(buf, "; ", sz);
            strlcat(buf, table[i].name, sz);
            first = false;
        }
    }
    if(first) strlcpy(buf, "?", sz);
}

// ============================================================================
// Process a completed frame
// ============================================================================

static void subghz_protocol_code_alarm_process(SubGhzProtocolDecoderCodeAlarm* instance) {
    if(instance->decoder.decode_count_bit != CODE_ALARM_BIT_COUNT) {
        return;
    }

    uint64_t data = instance->decoder.decode_data & CODE_ALARM_DATA_MASK;

    CodeAlarmFrame frame;
    if(!subghz_protocol_code_alarm_parse(data, &frame)) {
        return;
    }

    instance->generic.data = data;
    instance->generic.data_count_bit = CODE_ALARM_BIT_COUNT;
    instance->generic.serial = frame.id;
    instance->generic.btn = frame.button;
    instance->generic.cnt = 0;

    FURI_LOG_D(TAG, "Frame: ID:%06lX Btn:%X", (unsigned long)frame.id, frame.button);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

static void* subghz_protocol_decoder_code_alarm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderCodeAlarm* instance = malloc(sizeof(SubGhzProtocolDecoderCodeAlarm));
    instance->base.protocol = &subghz_protocol_code_alarm;
    instance->generic.protocol_name = subghz_protocol_code_alarm.name;
    return instance;
}

static void subghz_protocol_decoder_code_alarm_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_code_alarm_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;
    instance->decoder.parser_step = CodeAlarmDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

// ============================================================================
// Decoder: feed — OOK-Manchester slicer with reset-gap framing
//
// This protocol has NO magic sync word (its preamble is plain zeros), so the
// frame is delimited by the inter-frame reset gap. Each short/long half-bit is
// fed to the toolbox manchester_advance; a duration that is neither short nor
// long is treated as the reset gap: if exactly 60 bits were collected we run
// process()+callback, then clear state to start the next repeat. Alignment is
// pinned by the gap and re-confirmed by the 16-zero preamble check in parse().
// ============================================================================

static void subghz_protocol_decoder_code_alarm_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    if(DURATION_DIFF(duration, subghz_protocol_code_alarm_const.te_short) <=
       subghz_protocol_code_alarm_const.te_delta) {
        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    } else if(
        DURATION_DIFF(duration, subghz_protocol_code_alarm_const.te_long) <=
        subghz_protocol_code_alarm_const.te_delta) {
        event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }

    if(event == ManchesterEventReset) {
        /* Frame boundary (inter-frame gap or noise): flush a complete frame,
         * then reset the Manchester state and bit register for the next copy. */
        subghz_protocol_code_alarm_process(instance);
        manchester_advance(
            instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->decoder.parser_step = CodeAlarmDecoderStepDecode;
        return;
    }

    bool data;
    bool data_ok =
        manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data);

    if(data_ok) {
        data = !data; // Invert for IEEE convention
        instance->decoder.decode_data = (instance->decoder.decode_data << 1) | (data ? 1ULL : 0ULL);
        if(instance->decoder.decode_count_bit < CODE_ALARM_MAX_BITS) {
            instance->decoder.decode_count_bit++;
        } else {
            /* Runaway stream without a gap: drop it and resynchronise. */
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / get_string
// ============================================================================

static uint8_t subghz_protocol_decoder_code_alarm_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_code_alarm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    subghz_protocol_decoder_code_alarm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_code_alarm_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_code_alarm_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderCodeAlarm* instance = context;

    /* Recompute fields from the stored payload so the display is correct after
     * a load-from-file as well as after a live decode. */
    CodeAlarmFrame frame = {0};
    bool ok = (instance->generic.data_count_bit == CODE_ALARM_BIT_COUNT) &&
              subghz_protocol_code_alarm_parse(
                  instance->generic.data & CODE_ALARM_DATA_MASK, &frame);

    uint32_t id;
    uint8_t button;
    if(ok) {
        id = frame.id;
        button = frame.button;
    } else {
        id = instance->generic.serial;
        button = instance->generic.btn;
    }

    char buttons[48];
    subghz_protocol_code_alarm_button_name(button, buttons, sizeof(buttons));

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%015llX\r\n"
        "Sn:%06lX Btn:%s\r\n"
        "Data:%02X%02X%02X%02X%02X",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)(instance->generic.data & CODE_ALARM_DATA_MASK),
        (unsigned long)id,
        buttons,
        frame.body[0],
        frame.body[1],
        frame.body[2],
        frame.body[3],
        frame.body[4]);
}
