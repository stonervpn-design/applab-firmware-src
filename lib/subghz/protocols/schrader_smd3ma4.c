#include "schrader_smd3ma4.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SchraderSMD3MA4"

// Port of rtl_433/src/devices/schraeder.c :: schrader_SMD3MA4_family_decode
// (Benjamin Larsson / rtl_433 contributors, GPL-2+). Model SCHRADER_SMD3MA4.
//
//   Schrader TPMS SMD3MA4 (Subaru). Sibling MRXNIS315G3/3039 (Nissan/Infiniti/
//   Renault) shares the same wire format at 315 MHz with pressure scale 0.25.
//
// Frequency:  433.92 MHz (SMD3MA4) / 315 MHz (NIS315G3)
// Modulation: OOK, PCM raw bits at 120 us, then Manchester-decoded data.
//             rtl_433: short_width = long_width = 120, reset_limit = 480.
//
// Framing (raw OOK bits):
//   Preamble  0xF5555555E  (36 raw bits: F, seven 5s, E)
//   rtl_433 searches the last 16 preamble bits 0x555E and keeps the final 2
//   bits ("10") as the FIRST Manchester data pair (bitpos += 14). Equivalently
//   the raw stream locks on the 14-bit tail 0b01010101010111 (0x1557), after
//   which every following raw pair is a Manchester data symbol.
//
// Manchester (rtl_433 bitbuffer_manchester_decode + bitbuffer_invert):
//   each pair (h1,h2) must transition (h1 != h2); the library appends h2 then
//   the whole buffer is inverted, so the recovered data bit = !h2 = h1 (the
//   FIRST half of every transitioning pair).
//
// Data: 38 bits = 1 fixed(1) + 3 flags + 24 id + 8 pressure + 2 checksum.
//   Byte view b[0..4] (b[4] holds 6 valid bits, low 2 are padding zeros):
//     1FFFSSSS SSSSSSSS SSSSSSSS SSSSPPPP PPPPXX..
//   flags     = (b[0] & 0x70) >> 4          (0x7 = normal driving)
//   serial_id = (b[0]&0x0f)<<20 | b[1]<<12 | b[2]<<4 | b[3]>>4   (24-bit)
//   pressure  = (b[3]&0x0f)<<4 | b[4]>>4     (8-bit); PSI = pressure * 0.2
//   No temperature field is transmitted.
//
// Checksum (rtl_433 "PARITY"): add every 2-bit group of b[0..4] (including the
//   fixed leading 1-bit) modulo 4; a valid frame yields (sum & 3) == 1.

#define SMD3MA4_RAW_SYNC   0x1557U // 14-bit preamble tail 0b01010101010111
#define SMD3MA4_DATA_BITS  38
#define SMD3MA4_SYNC_MASK  0x3FFFU // low 14 bits

static const SubGhzBlockConst tpms_protocol_schrader_smd3ma4_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 55,
    .min_count_bit_for_found = SMD3MA4_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // OOK PCM: raw half-symbols are hunted for the preamble tail, then paired
    // up as Manchester data (mirrors rtl_433 bitbuffer_search +
    // bitbuffer_manchester_decode).
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble locked, collecting Manchester data
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
} TPMSProtocolDecoderSchraderSMD3MA4;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderSchraderSMD3MA4;

const SubGhzProtocolDecoder tpms_protocol_schrader_smd3ma4_decoder = {
    .alloc = tpms_protocol_decoder_schrader_smd3ma4_alloc,
    .free = tpms_protocol_decoder_schrader_smd3ma4_free,
    .feed = tpms_protocol_decoder_schrader_smd3ma4_feed,
    .reset = tpms_protocol_decoder_schrader_smd3ma4_reset,
    .get_hash_data = tpms_protocol_decoder_schrader_smd3ma4_get_hash_data,
    .serialize = tpms_protocol_decoder_schrader_smd3ma4_serialize,
    .deserialize = tpms_protocol_decoder_schrader_smd3ma4_deserialize,
    .get_string = tpms_protocol_decoder_schrader_smd3ma4_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_schrader_smd3ma4_encoder = {
    .alloc = tpms_protocol_encoder_schrader_smd3ma4_alloc,
    .free = tpms_protocol_encoder_schrader_smd3ma4_free,
    .deserialize = tpms_protocol_encoder_schrader_smd3ma4_deserialize,
    .stop = tpms_protocol_encoder_schrader_smd3ma4_stop,
    .yield = tpms_protocol_encoder_schrader_smd3ma4_yield,
};

const SubGhzProtocol tpms_protocol_schrader_smd3ma4 = {
    .name = TPMS_PROTOCOL_SCHRADER_SMD3MA4_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_schrader_smd3ma4_decoder,
    .encoder = &tpms_protocol_schrader_smd3ma4_encoder,
};

void* tpms_protocol_decoder_schrader_smd3ma4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSchraderSMD3MA4* instance =
        calloc(1, sizeof(TPMSProtocolDecoderSchraderSMD3MA4));
    instance->base.protocol = &tpms_protocol_schrader_smd3ma4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_schrader_smd3ma4_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_schrader_smd3ma4_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->have_half = false;
    instance->first_half = 0;
}

// Extract the five payload bytes b[0..4] from the 38-bit MSB-first decode_data.
// b[4] holds only 6 valid bits (PPPP XX); the low 2 bits are padding zeros.
static void smd3ma4_get_bytes(uint64_t data, uint8_t b[5]) {
    b[0] = (uint8_t)((data >> 30) & 0xFF);
    b[1] = (uint8_t)((data >> 22) & 0xFF);
    b[2] = (uint8_t)((data >> 14) & 0xFF);
    b[3] = (uint8_t)((data >> 6) & 0xFF);
    b[4] = (uint8_t)((data & 0x3F) << 2);
}

// rtl_433 checksum: sum of every 2-bit group of b[0..4] (mod 4) must equal 1.
static uint8_t smd3ma4_group_sum(const uint8_t b[5]) {
    uint8_t sum = 0;
    for(int i = 0; i < 5; i++) {
        sum += ((b[i] >> 0) & 0x3) + ((b[i] >> 2) & 0x3) + ((b[i] >> 4) & 0x3) +
               ((b[i] >> 6) & 0x3);
    }
    return sum;
}

static bool smd3ma4_check_crc(uint64_t data) {
    uint8_t b[5];
    smd3ma4_get_bytes(data, b);
    // reject all-zero data (rtl_433 DECODE_FAIL_SANITY)
    if(!b[0] && !b[1] && !b[2] && !b[3]) return false;
    return (smd3ma4_group_sum(b) & 0x3) == 1;
}

static void smd3ma4_analyze(TPMSBlockGeneric* instance) {
    uint8_t b[5];
    smd3ma4_get_bytes(instance->data, b);

    // 24-bit serial ID (rtl_433 serial_id).
    instance->id = ((uint32_t)(b[0] & 0x0f) << 20) | ((uint32_t)b[1] << 12) |
                   ((uint32_t)b[2] << 4) | ((uint32_t)b[3] >> 4);

    // flags nibble: 0x7 = normal driving, 0x0 = learn, 0x3 = alarm, 0x5 = wakeup.
    // There is no documented battery-low bit, so keep battery OK (0).
    instance->battery_low = 0;

    // Pressure: raw * 0.2 = PSI (SMD3MA4). TPMSBlockGeneric.pressure is stored in
    // bar (1 PSI = 0.0689476 bar), matching the rest of the TPMS suite.
    uint16_t pressure_raw = (uint16_t)(((b[3] & 0x0f) << 4) | (b[4] >> 4));
    instance->pressure = (float)pressure_raw * 0.2f * 0.0689476f;

    // No temperature is transmitted by this protocol.
    instance->temperature = 0.0f;
}

// Consume one raw half-symbol bit: hunt the 14-bit preamble tail 0x1557, then
// Manchester-decode the 38-bit payload (data bit = first half of each
// transitioning pair; see file header for the rtl_433 equivalence).
static void smd3ma4_emit_raw_bit(TPMSProtocolDecoderSchraderSMD3MA4* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if((instance->raw_shift & SMD3MA4_SYNC_MASK) == SMD3MA4_RAW_SYNC) {
            instance->in_data = true;
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }

    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        // No transition: end of frame / garbage -> resync on the preamble.
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->first_half; // = !second_half, matches rtl_433 invert
    subghz_protocol_blocks_add_bit(&instance->decoder, data_bit);
    if(instance->decoder.decode_count_bit == SMD3MA4_DATA_BITS) {
        if(smd3ma4_check_crc(instance->decoder.decode_data)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            smd3ma4_analyze(&instance->generic);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        } else {
            FURI_LOG_D(TAG, "checksum failed");
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_schrader_smd3ma4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;

    const uint32_t te = tpms_protocol_schrader_smd3ma4_const.te_short; // 120us half-symbol

    // A gap (anything much longer than the 4-half-symbol preamble/frame runs)
    // ends the burst. The longest legitimate run on the wire is the 0xF preamble
    // nibble and the preamble/data boundary, both 4 half-symbols (480us).
    if(duration >= te * 6) {
        tpms_protocol_decoder_schrader_smd3ma4_reset(instance);
        return;
    }
    // Expand this pulse into its run of equal-level raw half-symbols. Runs up to
    // 4 occur at the 0xF nibble and the preamble->data boundary (0x...5E), so do
    // not clamp below that.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 5) n = 5;
    for(uint32_t k = 0; k < n; k++) {
        smd3ma4_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_schrader_smd3ma4_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_schrader_smd3ma4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_schrader_smd3ma4_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_schrader_smd3ma4_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_schrader_smd3ma4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderSMD3MA4* instance = context;
    uint8_t b[5];
    smd3ma4_get_bytes(instance->generic.data, b);
    int flags = (b[0] & 0x70) >> 4;
    double psi = (double)instance->generic.pressure / 0.0689476;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Fl:%X\r\n"
        "%3.1f PSI Bar:%2.2f",
        instance->generic.protocol_name,
        instance->generic.id,
        flags,
        psi,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// OOK PCM raw preamble then Manchester data, reproducing the exact wire format:
//   gap + raw 0xF5555555 + "11"  (bits 0..33 of 0xF5555555E)
//   + 38 Manchester data bits, MSB first, first bit = fixed 1 (its pair "10"
//     completes the trailing 0xE of the preamble).
// Manchester data bit convention (matches the decoder = first half is the data
// bit): bit 1 -> HIGH,LOW ; bit 0 -> LOW,HIGH.
//
// Worst-case entries: 1 gap + 34 preamble halves + 2*38 data halves + 1 flush
// = 112 before run-length merging.
#define TPMS_SCHRADER_SMD3MA4_ENCODER_UPLOAD_SIZE 160

void* tpms_protocol_encoder_schrader_smd3ma4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderSchraderSMD3MA4* instance =
        malloc(sizeof(TPMSProtocolEncoderSchraderSMD3MA4));
    instance->base.protocol = &tpms_protocol_schrader_smd3ma4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SCHRADER_SMD3MA4_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_schrader_smd3ma4_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderSchraderSMD3MA4* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_schrader_smd3ma4_stop(void* context) {
    TPMSProtocolEncoderSchraderSMD3MA4* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the 38-bit payload from generic.data and RECOMPUTE the 2-bit parity
// checksum so an edited/replayed frame stays valid. Returns the payload MSB-first
// in the low 38 bits.
static uint64_t tpms_protocol_encoder_schrader_smd3ma4_build_frame(TPMSBlockGeneric* generic) {
    uint8_t b[5];
    smd3ma4_get_bytes(generic->data, b);
    // Zero the checksum group (b[4] bits[3:2]) then solve for it: with XX=0 the
    // group sum is S, and we need (S + XX) & 3 == 1 -> XX = (1 - S) & 3.
    b[4] &= 0xF0; // keep the 4 pressure-low bits (bits[7:4]); clear XX + padding
    uint8_t s = smd3ma4_group_sum(b);
    uint8_t xx = (uint8_t)((1 - s) & 0x3);
    b[4] |= (uint8_t)(xx << 2);

    uint64_t data = ((uint64_t)b[0] << 30) | ((uint64_t)b[1] << 22) | ((uint64_t)b[2] << 14) |
                    ((uint64_t)b[3] << 6) | ((uint64_t)(b[4] >> 2));
    return data;
}

static bool
    tpms_protocol_encoder_schrader_smd3ma4_get_upload(TPMSProtocolEncoderSchraderSMD3MA4* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_schrader_smd3ma4_const.te_short;

    uint64_t data = tpms_protocol_encoder_schrader_smd3ma4_build_frame(&instance->generic);

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

#define MAN_BIT(bit)         \
    do {                     \
        if(bit) {            \
            EMIT(true, te);  \
            EMIT(false, te); \
        } else {             \
            EMIT(false, te); \
            EMIT(true, te);  \
        }                    \
    } while(0)

    // Inter-frame gap so consecutive repeats are framed / the decoder resyncs.
    EMIT(false, te * 50);

    // Raw preamble bits 0..33 of 0xF5555555E: 0xF5555555 then "11". Emitted as
    // individual raw half-symbols (NOT Manchester); the decoder locks on the
    // 14-bit tail 0x1557. The last data pair below (fixed bit 1 -> "10") supplies
    // the trailing 0xE.
    static const uint8_t preamble[4] = {0xF5, 0x55, 0x55, 0x55};
    for(int i = 0; i < 4; i++) {
        for(int bpos = 7; bpos >= 0; bpos--) {
            EMIT(((preamble[i] >> bpos) & 0x1) ? true : false, te);
        }
    }
    // Two extra sync highs (bits 32,33 = "11").
    EMIT(true, te);
    EMIT(true, te);

    // 38 Manchester data bits, MSB first.
    for(int i = SMD3MA4_DATA_BITS - 1; i >= 0; i--) {
        MAN_BIT((data >> i) & 0x1);
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

void tpms_protocol_schrader_smd3ma4_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    // 24-bit serial ID.
    uint32_t serial = generic->id & 0x00FFFFFFUL;

    // Pressure: bar -> raw (inverse of analyze: bar = raw * 0.2 * 0.0689476).
    int32_t pressure_raw = (int32_t)(generic->pressure / (0.2f * 0.0689476f) + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0xFF) pressure_raw = 0xFF;

    const uint8_t flags = 0x7; // normal driving

    uint8_t b[5];
    b[0] = (uint8_t)(0x80 | (flags << 4) | ((serial >> 20) & 0x0f));
    b[1] = (uint8_t)((serial >> 12) & 0xFF);
    b[2] = (uint8_t)((serial >> 4) & 0xFF);
    b[3] = (uint8_t)(((serial & 0x0f) << 4) | (((uint32_t)pressure_raw >> 4) & 0x0f));
    b[4] = (uint8_t)(((uint32_t)pressure_raw & 0x0f) << 4); // XX solved below

    // Solve the 2-bit parity checksum: need (group_sum & 3) == 1.
    uint8_t s = smd3ma4_group_sum(b);
    uint8_t xx = (uint8_t)((1 - s) & 0x3);
    b[4] |= (uint8_t)(xx << 2);

    generic->data = ((uint64_t)b[0] << 30) | ((uint64_t)b[1] << 22) | ((uint64_t)b[2] << 14) |
                    ((uint64_t)b[3] << 6) | ((uint64_t)(b[4] >> 2));
    generic->data_count_bit = SMD3MA4_DATA_BITS;
}

SubGhzProtocolStatus tpms_protocol_encoder_schrader_smd3ma4_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderSchraderSMD3MA4* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_schrader_smd3ma4_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_SCHRADER_SMD3MA4_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_schrader_smd3ma4_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_schrader_smd3ma4_yield(void* context) {
    TPMSProtocolEncoderSchraderSMD3MA4* instance = context;

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
