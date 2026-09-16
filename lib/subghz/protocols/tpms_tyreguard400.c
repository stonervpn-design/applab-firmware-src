#include "tpms_tyreguard400.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTyreGuard400"

// Port of rtl_433/src/devices/tpms_tyreguard400.c (R. Alvergnat 2022, GPL-2+)
// "TyreGuard 400 TPMS" from Davies Craig.
//
// Frequency: 434.1 MHz.
// Modulation: OOK, Manchester (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT),
//   short = long = 100 us half-symbol, reset_limit 500 us.
//
// The whole frame is Manchester-encoded. After Manchester-decoding, rtl_433
// searches the 28-bit frame sync 0xFD5FD5F (top 28 bits of the byte pattern
// {0xFD,0x5F,0xD5,0xF0}) and then reads a 88-bit (11 byte) message that STARTS
// at the sync, so the sync is part of the CRC-covered message:
//
//   b[0] b[1] b[2]  = 0xFD 0x5F 0xD5           (frame sync, bytes 0..2)
//   b[3] high nibble = 0xF                      (frame sync, 4 more bits)
//   b[3] low nibble + b[4] b[5] b[6] = 28-bit ID
//   b[7]           = pressure low 8 bits (kPa)
//   b[8]           = temperature, degC + 40
//   b[9]           = flags:  0x70 -> pressure high 3 bits (kPa bits 8..10),
//                            0x08 -> ack leaking, 0x03 -> leaking / peering req
//   b[10]          = CRC-8 (poly 0x31, init 0xDD) so crc8(b[0..10]) == 0
//
//   pressure_kPa = b[7] | ((b[9] & 0x70) << 4)   (11 bit, we store bar = kPa/100)
//   temp_C       = b[8] - 40
//
// The 88-bit frame spills its top 24 bits (b[0..2]) into head_64_bit via
// subghz_protocol_blocks_add_to_128_bit; b[3..10] land in decode_data (64 bit),
// which is what we serialize as generic.data. The sync bytes b[0..2] are fixed
// constants the encoder re-synthesizes, so they need not be stored.

#define TYREGUARD400_DATA_BITS 88
#define TYREGUARD400_SYNC      0x0FD5FD5FUL // 28-bit Manchester-decoded frame sync
#define TYREGUARD400_SYNC_MASK 0x0FFFFFFFUL
#define TYREGUARD400_SYNC_BITS 28

static const SubGhzBlockConst tpms_protocol_tpms_tyreguard400_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 45,
    .min_count_bit_for_found = TYREGUARD400_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // OOK Manchester: feed() expands each pulse into raw half-symbol bits, which
    // are paired into Manchester data bits. Until the 28-bit sync is seen in the
    // DECODED bit stream we only feed a rolling shift register; once matched we
    // prefill the accumulator with the sync and collect the remaining bits to 88.
    bool have_half; // holding the first half-symbol of the current pair
    uint8_t first_half; // the first half-symbol value
    bool in_data; // true once the sync matched, accumulating the 88-bit frame
    uint32_t sync_shift; // rolling 28-bit window of decoded bits for sync hunt
    uint64_t head_64_bit; // frame bits above 64 (b[0..2])
} TPMSProtocolDecoderTyreGuard400;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderTyreGuard400;

const SubGhzProtocolDecoder tpms_protocol_tpms_tyreguard400_decoder = {
    .alloc = tpms_protocol_decoder_tpms_tyreguard400_alloc,
    .free = tpms_protocol_decoder_tpms_tyreguard400_free,
    .feed = tpms_protocol_decoder_tpms_tyreguard400_feed,
    .reset = tpms_protocol_decoder_tpms_tyreguard400_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_tyreguard400_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_tyreguard400_serialize,
    .deserialize = tpms_protocol_decoder_tpms_tyreguard400_deserialize,
    .get_string = tpms_protocol_decoder_tpms_tyreguard400_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_tyreguard400_encoder = {
    .alloc = tpms_protocol_encoder_tpms_tyreguard400_alloc,
    .free = tpms_protocol_encoder_tpms_tyreguard400_free,
    .deserialize = tpms_protocol_encoder_tpms_tyreguard400_deserialize,
    .stop = tpms_protocol_encoder_tpms_tyreguard400_stop,
    .yield = tpms_protocol_encoder_tpms_tyreguard400_yield,
};

const SubGhzProtocol tpms_protocol_tpms_tyreguard400 = {
    .name = TPMS_PROTOCOL_TYREGUARD400_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_tyreguard400_decoder,
    .encoder = &tpms_protocol_tpms_tyreguard400_encoder,
};

void* tpms_protocol_decoder_tpms_tyreguard400_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTyreGuard400* instance = calloc(1, sizeof(TPMSProtocolDecoderTyreGuard400));
    instance->base.protocol = &tpms_protocol_tpms_tyreguard400;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_tyreguard400_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_tyreguard400_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->have_half = false;
    instance->first_half = 0;
    instance->in_data = false;
    instance->sync_shift = 0;
    instance->head_64_bit = 0;
}

// Reconstruct the 11 message bytes from head_64_bit (b[0..2]) + decode_data
// (b[3..10]).
static void tyreguard400_get_bytes(TPMSProtocolDecoderTyreGuard400* instance, uint8_t b[11]) {
    b[0] = (uint8_t)((instance->head_64_bit >> 16) & 0xFF);
    b[1] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[2] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (uint8_t)((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF);
    }
}

static bool tyreguard400_check_crc(TPMSProtocolDecoderTyreGuard400* instance) {
    uint8_t b[11];
    tyreguard400_get_bytes(instance, b);
    // rtl_433: crc8(b, 11, 0x31, 0xDD) == 0 (CRC byte b[10] included).
    return subghz_protocol_blocks_crc8(b, 11, 0x31, 0xDD) == 0;
}

static void tyreguard400_analyze(TPMSProtocolDecoderTyreGuard400* instance) {
    uint8_t b[11];
    tyreguard400_get_bytes(instance, b);
    instance->generic.id =
        (uint32_t)(((b[3] & 0x0F) << 24) | (b[4] << 16) | (b[5] << 8) | b[6]);
    int pressure_kpa = b[7] | ((b[9] & 0x70) << 4);
    instance->generic.pressure = (float)pressure_kpa * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int)b[8] - 40);
    // TyreGuard 400 has no documented battery-low bit; keep OK.
    instance->generic.battery_low = 0;
}

// Feed one decoded Manchester data bit into the sync hunt / frame accumulator.
static void tyreguard400_emit_data_bit(TPMSProtocolDecoderTyreGuard400* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->sync_shift =
            ((instance->sync_shift << 1) | bit) & TYREGUARD400_SYNC_MASK;
        if(instance->sync_shift == TYREGUARD400_SYNC) {
            // Sync matched: start a fresh 88-bit frame, prefilled with the 28
            // sync bits so b[0..2]+b[3] high-nibble are in place for the CRC.
            instance->in_data = true;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
            for(int i = TYREGUARD400_SYNC_BITS - 1; i >= 0; i--) {
                subghz_protocol_blocks_add_to_128_bit(
                    &instance->decoder,
                    (uint8_t)((TYREGUARD400_SYNC >> i) & 1),
                    &instance->head_64_bit);
            }
        }
        return;
    }
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == TYREGUARD400_DATA_BITS) {
        if(tyreguard400_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            tyreguard400_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->sync_shift = 0;
    }
}

// Consume one raw half-symbol bit: pair adjacent half-symbols into a Manchester
// data bit (a valid pair transitions; the data bit is the first half — the same
// convention the encoder produces). A non-transitioning pair means loss of sync.
static void tyreguard400_emit_half(TPMSProtocolDecoderTyreGuard400* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        // No mid-symbol transition: drop back to hunting from scratch.
        instance->in_data = false;
        instance->sync_shift = 0;
        return;
    }
    tyreguard400_emit_data_bit(instance, instance->first_half);
}

void tpms_protocol_decoder_tpms_tyreguard400_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;

    const uint32_t te = tpms_protocol_tpms_tyreguard400_const.te_short; // 100us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_tyreguard400_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        tyreguard400_emit_half(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_tyreguard400_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_tyreguard400_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_tyreguard400_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_tyreguard400_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_tyreguard400_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%d\r\n"
        "Temp:%2.0f C Bar:%2.1f kPa:%3.0f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure,
        (double)(instance->generic.pressure * 100.0f));
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// OOK Manchester, non-inverting (data bit = first half-symbol), matching the
// decoder:
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
// Frame: inter-frame gap (reset) + 88 Manchester data bits (b[0..10], MSB first).
// The 88-bit frame includes the fixed sync b[0..2]=0xFD 0x5F 0xD5 and b[3] high
// nibble = 0xF, which the decoder hunts for in the Manchester-decoded stream.
//
// generic.data holds b[3..10]; b[0..2] are re-synthesized constants and b[10]
// (CRC) is recomputed in build_frame.
//
// Worst-case entries: 1 gap + 2*88 half-symbols + 1 flush = 178.
#define TPMS_TYREGUARD400_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_tpms_tyreguard400_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTyreGuard400* instance = malloc(sizeof(TPMSProtocolEncoderTyreGuard400));
    instance->base.protocol = &tpms_protocol_tpms_tyreguard400;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_TYREGUARD400_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_tyreguard400_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTyreGuard400* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_tyreguard400_stop(void* context) {
    TPMSProtocolEncoderTyreGuard400* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 11-byte frame: b[0..2] fixed sync, b[3..9] from generic.data,
// b[10] recomputed CRC-8 (poly 0x31, init 0xDD over b[0..9]).
static void tpms_protocol_encoder_tpms_tyreguard400_build_frame(
    TPMSBlockGeneric* generic,
    uint8_t b[11]) {
    b[0] = 0xFD;
    b[1] = 0x5F;
    b[2] = 0xD5;
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    // Force the sync high nibble of b[3] and recompute the CRC so the frame is
    // self-consistent regardless of the stored bytes.
    b[3] = (uint8_t)(0xF0 | (b[3] & 0x0F));
    b[10] = subghz_protocol_blocks_crc8(b, 10, 0x31, 0xDD);
}

static bool tpms_protocol_encoder_tpms_tyreguard400_get_upload(
    TPMSProtocolEncoderTyreGuard400* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_tyreguard400_const.te_short;

    uint8_t frame[11];
    tpms_protocol_encoder_tpms_tyreguard400_build_frame(&instance->generic, frame);

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
// decoder takes the data bit from the first half of each transitioning pair).
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

    // Inter-frame gap so consecutive repeats frame cleanly and the decoder
    // establishes Manchester phase from the reset.
    EMIT(false, te * 50);

    // 88 data bits, MSB first (frame[0]..frame[10]).
    for(int i = 0; i < TYREGUARD400_DATA_BITS; i++) {
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

void tpms_protocol_tpms_tyreguard400_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    // pressure: bar -> kPa raw (11 bit), split low 8 in b[7] and high 3 in b[9].
    int pressure_kpa = (int)(generic->pressure * 100.0f + 0.5f);
    if(pressure_kpa < 0) pressure_kpa = 0;
    if(pressure_kpa > 0x7FF) pressure_kpa = 0x7FF;

    int temp_raw = (int)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f)) + 40;
    if(temp_raw < 0) temp_raw = 0;
    if(temp_raw > 0xFF) temp_raw = 0xFF;

    uint8_t b[11];
    b[0] = 0xFD;
    b[1] = 0x5F;
    b[2] = 0xD5;
    b[3] = (uint8_t)(0xF0 | ((generic->id >> 24) & 0x0F));
    b[4] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[5] = (uint8_t)((generic->id >> 8) & 0xFF);
    b[6] = (uint8_t)(generic->id & 0xFF);
    b[7] = (uint8_t)(pressure_kpa & 0xFF);
    b[8] = (uint8_t)temp_raw;
    b[9] = (uint8_t)(((pressure_kpa >> 8) & 0x07) << 4); // flags: pressure high bits, rest 0
    b[10] = subghz_protocol_blocks_crc8(b, 10, 0x31, 0xDD);

    // generic.data holds b[3..10] (the low 64 bits of the 88-bit frame).
    uint64_t data = 0;
    for(int i = 3; i < 11; i++) data = (data << 8) | b[i];
    generic->data = data;
    generic->data_count_bit = TYREGUARD400_DATA_BITS;
}

SubGhzProtocolStatus tpms_protocol_encoder_tpms_tyreguard400_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTyreGuard400* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_tyreguard400_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_TYREGUARD400_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_tyreguard400_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_tyreguard400_yield(void* context) {
    TPMSProtocolEncoderTyreGuard400* instance = context;

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
