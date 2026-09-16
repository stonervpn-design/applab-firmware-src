#include "tpms_mercedes_benz.h"
#include "../blocks/generic.h"

#define TAG "TPMSMercedesBenz"

// Port of rtl_433/src/devices/tpms_mercedes_benz.c
//   (C) 2026 Bruno OCTAU (ProfBoc75), GPL-2+. rtl_433 issue #3539.
//
// Mercedes Benz Sprinter 4500 TPMS sensor (2025). 433.92 MHz FSK,
// Manchester-coded (FSK_PULSE_MANCHESTER_ZEROBIT), 25 us half-bit.
//
// rtl_433 works on the Manchester-DECODED bitstream: it searches the 12-bit
// preamble 0x002 and extracts the 80 bits (10 bytes) that follow. Because the
// whole frame is Manchester, on-air the preamble 0x002 is itself Manchester-coded.
// This on-device port mirrors the sibling ports (tpms_citroen / tpms_bmw): it hunts
// a fixed sync pattern in the RAW half-symbol stream, which establishes Manchester
// pair alignment, then Manchester-decodes the 80-bit payload.
//
// The sync we hunt is the tail of the Manchester-encoded preamble. With the G.E.
// Thomas convention used here (bit 1 -> raw high,low ; bit 0 -> raw low,high) the
// 12-bit preamble 0x002 = 0000_0000_0010b encodes to the raw half-symbols
//   (01)x10 (10) (01)  -> ...0101 0101 0101 1001 -> low 16 raw bits = 0x5559
// so a raw match of 0x5559 lands pair-aligned at the first payload half-symbol.
// The RF-inverted demod yields ~0x5559 = 0xAAA6, which we also accept (and then
// invert every raw bit) so either FSK deviation sign decodes.
//
// Wire format (after Manchester-decoding), 10 bytes / 80 bits:
//   ST II II II II PP TT CC FF XX
//   b[0]      = ST family/state (0x83 stationary, 0xa3 moving)
//   b[1..4]   = 32-bit ID / serial (b[1] MSB)
//   b[5]      = PP pressure raw       -> PSI = PP / 2.75
//   b[6]      = TT temperature raw    -> C   = TT - 51
//   b[7]      = CC counter (low 5 bits) + flags1 (b[7] >> 5)
//   b[8]      = FF status flags
//   b[9]      = XX CRC-8 of b[0..8], poly 0x2f, init 0xaa, final XOR 0x00
//              (residue of crc8 over all 10 bytes incl. CRC == 0)
//
// The 80-bit payload's top 16 bits (b[0..1]) spill into head_64_bit via
// subghz_protocol_blocks_add_to_128_bit; the low 64 bits (b[2..9]) live in
// decode_data / generic.data.

#define MB_PREAMBLE     0x5559U // raw tail of Manchester(0x002)
#define MB_PREAMBLE_INV 0xAAA6U // (uint16_t)~0x5559, RF-inverted polarity
#define MB_DATA_BITS    80 // 10 bytes
#define MB_STATE_STAT   0x83
#define MB_STATE_MOVE   0xa3

static const SubGhzBlockConst tpms_protocol_tpms_mercedes_benz_const = {
    .te_short = 25, // one Manchester half-symbol
    .te_long = 50, // two merged half-symbols
    .te_delta = 12,
    .min_count_bit_for_found = MB_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t raw_shift; // rolling window of raw half-symbol bits for the sync hunt
    bool in_data; // true once the sync matched, collecting Manchester data
    bool invert; // RF polarity chosen by which sync matched
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // payload bits above 64 (80-bit frame)
} TPMSProtocolDecoderMercedesBenz;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderMercedesBenz;

const SubGhzProtocolDecoder tpms_protocol_tpms_mercedes_benz_decoder = {
    .alloc = tpms_protocol_decoder_tpms_mercedes_benz_alloc,
    .free = tpms_protocol_decoder_tpms_mercedes_benz_free,
    .feed = tpms_protocol_decoder_tpms_mercedes_benz_feed,
    .reset = tpms_protocol_decoder_tpms_mercedes_benz_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_mercedes_benz_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_mercedes_benz_serialize,
    .deserialize = tpms_protocol_decoder_tpms_mercedes_benz_deserialize,
    .get_string = tpms_protocol_decoder_tpms_mercedes_benz_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_mercedes_benz_encoder = {
    .alloc = tpms_protocol_encoder_tpms_mercedes_benz_alloc,
    .free = tpms_protocol_encoder_tpms_mercedes_benz_free,
    .deserialize = tpms_protocol_encoder_tpms_mercedes_benz_deserialize,
    .stop = tpms_protocol_encoder_tpms_mercedes_benz_stop,
    .yield = tpms_protocol_encoder_tpms_mercedes_benz_yield,
};

const SubGhzProtocol tpms_protocol_tpms_mercedes_benz = {
    .name = TPMS_PROTOCOL_MERCEDES_BENZ_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_mercedes_benz_decoder,
    .encoder = &tpms_protocol_tpms_mercedes_benz_encoder,
};

void* tpms_protocol_decoder_tpms_mercedes_benz_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderMercedesBenz* instance =
        calloc(1, sizeof(TPMSProtocolDecoderMercedesBenz));
    instance->base.protocol = &tpms_protocol_tpms_mercedes_benz;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_mercedes_benz_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_mercedes_benz_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->invert = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

static void mb_get_bytes(TPMSProtocolDecoderMercedesBenz* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit low 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF);
    }
}

static void mb_analyze(TPMSProtocolDecoderMercedesBenz* instance, const uint8_t* b) {
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure: rtl_433 PSI = b[5] / 2.75; store as bar (1 PSI = 0.0689476 bar).
    instance->generic.pressure = ((float)b[5] / 2.75f) * 0.0689476f;
    // Temperature: C = b[6] - 51.
    instance->generic.temperature = (float)((int)b[6] - 51);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Consume one raw half-symbol bit: hunt the raw sync (0x5559, or the
// RF-inverted 0xAAA6), then Manchester-decode the 80-bit payload. Mirrors
// tpms_citroen/tpms_bmw: the sync is searched in the raw bits and only the data is
// Manchester. rtl_433 (G.E. Thomas) takes the FIRST half of each transitioning
// pair as the data bit (invert=false for the 0x5559 sync).
static void mb_emit_raw_bit(TPMSProtocolDecoderMercedesBenz* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == MB_PREAMBLE || instance->raw_shift == MB_PREAMBLE_INV) {
            instance->in_data = true;
            instance->invert = (instance->raw_shift == MB_PREAMBLE_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the first (polarity-corrected)
    // half. No transition = end/garbage -> resync on the sync pattern.
    uint8_t r = (uint8_t)(bit ^ (instance->invert ? 1 : 0));
    if(!instance->have_half) {
        instance->first_half = r;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == r) {
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->first_half;
    // 80-bit accumulation: add_to_128_bit shifts the top bits into head_64_bit.
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == MB_DATA_BITS) {
        uint8_t b[10];
        mb_get_bytes(instance, b);
        // CRC-8 poly 0x2f init 0xaa over b[0..8]; residue over all 10 bytes == 0.
        bool crc_ok = (subghz_protocol_blocks_crc8(b, 10, 0x2F, 0xAA) == 0);
        // Family/state sanity (rtl_433 DECODE_FAIL_SANITY otherwise).
        bool state_ok = (b[0] == MB_STATE_STAT || b[0] == MB_STATE_MOVE);
        if(crc_ok && state_ok) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            mb_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_tpms_mercedes_benz_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;

    const uint32_t te = tpms_protocol_tpms_mercedes_benz_const.te_short; // 25us half-symbol
    // Pure Manchester: runs never exceed two half-symbols. A longer pulse/gap ends
    // the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_mercedes_benz_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        mb_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_mercedes_benz_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_mercedes_benz_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_mercedes_benz_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_mercedes_benz_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_mercedes_benz_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderMercedesBenz* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer + Encoder (TX)
// -----------------------------------------------------------------------------
//
// generic.data carries the low 64 bits of the 80-bit frame = b[2..9]. b[0]=ST
// and b[1]=top ID byte are recovered by build_frame (ST fixed 0x83 stationary,
// b[1] from generic.id). PP/TT are packed from pressure/temperature; CC/FF flags
// are crafted 0 (preserved from generic.data on replay); XX (CRC) is recomputed.
//
// Manchester: G.E. Thomas (like tpms_bmw). The decoded data bit equals the FIRST
// half-symbol of each transitioning pair, so bit 1 -> raw high,low ; bit 0 -> raw
// low,high. The 12-bit preamble 0x002 is emitted Manchester-coded too (whole frame
// is Manchester), so its raw tail is the 0x5559 the decoder hunts.
//
// Worst-case entries: 1 gap + 2*(12 preamble + 80 data) + 1 tail = 186.
#define TPMS_MERCEDES_BENZ_ENCODER_UPLOAD_SIZE 240
#define MB_FRAME_BYTES                         10
#define MB_PREAMBLE_BITS                       12
#define MB_PREAMBLE_VALUE                      0x002U

void tpms_protocol_tpms_mercedes_benz_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar = (PP / 2.75) * 0.0689476  ->  PP = bar / 0.0689476 * 2.75
    int p_raw = (int)(g->pressure / 0.0689476f * 2.75f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // temperature: C = TT - 51  ->  TT = C + 51
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 51;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    // generic.data holds b[2..9]. build_frame recovers b[0]=ST and b[1]=id>>24,
    // then recomputes b[9]=CRC. Only b[5]=PP, b[6]=TT are (re)written here; the
    // low 24 ID bits (b[2..4]) come from generic.id; CC/FF flags start at 0.
    uint8_t b[8]; // = frame b[2..9]
    b[0] = (uint8_t)((g->id >> 16) & 0xFF); // b[2]
    b[1] = (uint8_t)((g->id >> 8) & 0xFF); // b[3]
    b[2] = (uint8_t)(g->id & 0xFF); // b[4]
    b[3] = (uint8_t)p_raw; // b[5] PP
    b[4] = (uint8_t)t; // b[6] TT
    b[5] = 0; // b[7] CC counter
    b[6] = 0; // b[8] FF flags
    b[7] = 0; // b[9] CRC (recomputed in build_frame)
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = MB_DATA_BITS;
}

void* tpms_protocol_encoder_tpms_mercedes_benz_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderMercedesBenz* instance =
        malloc(sizeof(TPMSProtocolEncoderMercedesBenz));
    instance->base.protocol = &tpms_protocol_tpms_mercedes_benz;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_MERCEDES_BENZ_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_mercedes_benz_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderMercedesBenz* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_mercedes_benz_stop(void* context) {
    TPMSProtocolEncoderMercedesBenz* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 10-byte frame. b[2..9] come from generic.data; b[1] from
// generic.id; b[0] is the ST state byte (0x83 stationary). Recompute the CRC so a
// modified / 0-PSI frame always validates.
static void tpms_protocol_encoder_tpms_mercedes_benz_build_frame(
    TPMSBlockGeneric* generic,
    uint8_t b[MB_FRAME_BYTES]) {
    b[0] = MB_STATE_STAT; // ST stationary; decoder validates it is 0x83 or 0xa3
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    // b[2..9] = low 64 bits of the frame, held in generic.data (MSB-first)
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    // CRC-8 poly 0x2f init 0xaa over b[0..8]; appending makes residue over 10 == 0
    b[9] = subghz_protocol_blocks_crc8(b, 9, 0x2F, 0xAA);
}

static bool tpms_protocol_encoder_tpms_mercedes_benz_get_upload(
    TPMSProtocolEncoderMercedesBenz* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_mercedes_benz_const.te_short;

    uint8_t frame[MB_FRAME_BYTES];
    tpms_protocol_encoder_tpms_mercedes_benz_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                  \
    do {                                                                \
        if(last_duration != 0 && last_level == (lvl)) {                 \
            last_duration += (dur);                                     \
        } else {                                                        \
            if(last_duration != 0) {                                    \
                if(index >= max) return false;                          \
                instance->encoder.upload[index++] =                     \
                    level_duration_make(last_level, last_duration);     \
            }                                                           \
            last_level = (lvl);                                         \
            last_duration = (dur);                                      \
        }                                                               \
    } while(0)

// G.E. Thomas Manchester: data bit = FIRST half-symbol (invert=false for 0x5559).
//   bit 1 -> raw high then low;  bit 0 -> raw low then high.
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

    // Inter-frame gap (also forces the decoder to a clean Reset state).
    EMIT(false, te * 50);

    // 12-bit preamble 0x002, Manchester-coded (the whole frame is Manchester). Its
    // raw tail is the 0x5559 the decoder hunts, which also sets pair alignment.
    for(int i = MB_PREAMBLE_BITS - 1; i >= 0; i--) {
        MAN_BIT((MB_PREAMBLE_VALUE >> i) & 0x1U);
    }

    // 80 data bits, MSB first (frame[0]..frame[9]), Manchester-coded.
    for(int i = 0; i < MB_DATA_BITS; i++) {
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

SubGhzProtocolStatus tpms_protocol_encoder_tpms_mercedes_benz_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderMercedesBenz* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_mercedes_benz_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_MERCEDES_BENZ_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_mercedes_benz_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_mercedes_benz_yield(void* context) {
    TPMSProtocolEncoderMercedesBenz* instance = context;

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
