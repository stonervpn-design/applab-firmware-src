#include "gm_rke.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * GM ABO1502T — General Motors car remote (RKE) decoder.
 *
 * Ported from the rtl_433 device "GM ABO1502T Car Remote" (gm_car_remote.c,
 * (C) 2024 Ethan Halsall). rtl_433 works on a pre-demodulated bitstream; here
 * we re-implement the same OOK-PPM demodulation as a pulse-duration state
 * machine over LevelDuration events, since the Flipper subghz worker hands us
 * one (level, duration) edge at a time and there is no ready-made bitbuffer.
 *
 * Physical layer (from rtl_433 r_device):
 *   - Carrier ~314.9 MHz, OOK (AM), band mapped to 315 flag here
 *   - Modulation OOK_PULSE_PPM: each pulse (mark) is a fixed short width and
 *     the GAP (space) after it carries the bit:
 *         short gap ~300us  -> bit 0
 *         long  gap ~500us  -> bit 1
 *   - reset_limit 20000us ends a row
 *
 * Frame (rtl_433 gm_car_remote_decode):
 *   - A long wake-up preamble precedes the payload and may be truncated, so
 *     rtl_433 aligns to the END of the row: offset = total_bits - 113, then
 *     extracts 112 bits (14 bytes). We mirror that: collect all PPM bits until
 *     a terminating gap, require >= 113 collected bits, and slice the 14-byte
 *     window starting at (count - 113), taking 112 bits (dropping the final
 *     bit exactly as rtl_433 does).
 *
 * Byte layout of the 14-byte (112-bit) window:
 *     PP xxxx cccc IIIIIIII SSSSSS EEEEEE CC
 *   byte[0]      : 0xFF wake-up validation (must equal 0xFF)
 *   byte[1]      : 8-bit unknown prefix (possibly high byte of ID)
 *   byte[2]      : upper nibble = button checksum, low 3 bits = button code
 *   byte[3..6]   : 32-bit ID (big-endian)
 *   byte[7..9]   : 24-bit sequence (rolling counter, unencrypted)
 *   byte[10..12] : 24-bit encrypted payload
 *   byte[13]     : 8-bit checksum of the whole payload
 *
 * Integrity (rejects false positives — this is the whole point of the gate):
 *   - button checksum: add_nibbles(bytes+2, 1): (byte[2]>>4)+(byte[2]&0xF);
 *     valid iff nonzero AND low nibble == 0.
 *   - full checksum: add_bytes(bytes+1, 13): sum of byte[1..13];
 *     valid iff nonzero AND low byte == 0.
 *
 * TX (ENABLE_EMULATE_FEATURE): ONE-SHOT verbatim replay only. byte[7..9] is a
 * cleartext rolling counter but byte[10..12] is a per-press encrypted
 * authenticator keyed under an unknown key that we cannot regenerate, so there
 * is NO next-code predictor (a bumped counter would emit codes the RX rejects).
 * The encoder rebuilds and re-sends the captured frame verbatim (byte[1] and
 * byte[10..12] are persisted alongside the generic id/seq/btn so the saved .sub
 * round-trips exactly). Replaying an already-used code is inert at the RX; this
 * is only useful against a code the owner has not yet spent.
 */

#define TAG "GmRke"

/* PPM timing (microseconds). Space (gap) after each pulse carries the bit. */
#define GM_RKE_TE_SHORT      300   /* short gap -> bit 0 */
#define GM_RKE_TE_LONG       500   /* long gap  -> bit 1 */
#define GM_RKE_SPACE_MIN     120   /* below this a gap is noise */
#define GM_RKE_BIT_THRESHOLD 400   /* midpoint: gap >= this -> 1, else 0 */
#define GM_RKE_SPACE_MAX     900   /* above this a gap terminates the frame */
#define GM_RKE_MARK_MIN      100   /* plausible pulse (mark) width lower bound */
#define GM_RKE_MARK_MAX      1000  /* plausible pulse (mark) width upper bound */

#define GM_RKE_FRAME_BITS 113      /* rtl_433: offset = total_bits - 113 */
#define GM_RKE_PAYLOAD_BITS 112    /* 14 bytes extracted */
#define GM_RKE_PAYLOAD_BYTES 14
#define GM_RKE_MIN_BITS 112        /* min_count_bit_for_found reported */

/* Rolling bit buffer. Wake-up preamble can be long; we only ever need the last
 * GM_RKE_FRAME_BITS, so cap the buffer and slide it down on overflow. */
#define GM_RKE_MAX_BITS  256
#define GM_RKE_KEEP_BITS 160       /* retained after an overflow slide */

static const SubGhzBlockConst subghz_protocol_gm_rke_const = {
    .te_short = GM_RKE_TE_SHORT,
    .te_long = GM_RKE_TE_LONG,
    .te_delta = 150,
    .min_count_bit_for_found = GM_RKE_MIN_BITS,
};

typedef enum {
    GmRkeDecoderStepReset = 0,
    GmRkeDecoderStepData,
} GmRkeDecoderStep;

struct SubGhzProtocolDecoderGmRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t bit_count;
    uint8_t bits[GM_RKE_MAX_BITS]; /* one 0/1 per PPM symbol, MSB-first order */

    /* byte[1] and byte[10..12]. Now persisted (see serialize/deserialize) so a
     * saved .sub can rebuild the exact captured frame for a one-shot replay.
     * 0 on an old file that predates persistence -> replay is checksum-valid but
     * not byte-identical to the original (a real RX rejects the stale code). */
    uint8_t unknown_prefix; /* byte[1] */
    uint32_t encrypted;     /* byte[10..12], 24-bit */
};

/* FlipperFormat keys for the two fields the generic block does not carry. */
#define GM_RKE_KEY_PREFIX "Prefix"
#define GM_RKE_KEY_ENC    "Enc"

#ifdef ENABLE_EMULATE_FEATURE
/* ---- One-shot verbatim replay encoder (ENCRYPTED_ROLLING: NO predictor) ----
 * GM RKE carries a 24-bit encrypted authenticator (byte[10..12]) keyed under an
 * unknown key that changes every press; we cannot regenerate it, so bumping the
 * cleartext counter would only produce codes the RX rejects. The most this
 * protocol supports is replaying the ONE captured frame verbatim. */

/* Wake-up preamble: short OOK pulses the real fob leads with. Any count works
 * for our own decoder (it end-aligns to the last 113 bits); a real GM RX wants
 * the wake burst present. */
#define GM_RKE_PREAMBLE_BITS 12
/* upload = (preamble + 112 payload + 1 trailing align bit + 1 terminator) pairs. */
#define GM_RKE_UPLOAD_SIZE   (2 * (GM_RKE_PREAMBLE_BITS + GM_RKE_PAYLOAD_BITS + 2))
/* Inter-frame / terminating gap: > GM_RKE_SPACE_MAX so the decoder ends the row. */
#define GM_RKE_TX_GUARD      15000

struct SubGhzProtocolEncoderGmRke {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t unknown_prefix; /* byte[1] from file */
    uint32_t encrypted;     /* byte[10..12] from file */
};

static void* subghz_protocol_encoder_gm_rke_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_encoder_gm_rke_free(void* context);
static SubGhzProtocolStatus
    subghz_protocol_encoder_gm_rke_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_encoder_gm_rke_stop(void* context);
static LevelDuration subghz_protocol_encoder_gm_rke_yield(void* context);
#endif


// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_gm_rke_decoder = {
    .alloc = subghz_protocol_decoder_gm_rke_alloc,
    .free = subghz_protocol_decoder_gm_rke_free,
    .feed = subghz_protocol_decoder_gm_rke_feed,
    .reset = subghz_protocol_decoder_gm_rke_reset,
    .get_hash_data = subghz_protocol_decoder_gm_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_gm_rke_serialize,
    .deserialize = subghz_protocol_decoder_gm_rke_deserialize,
    .get_string = subghz_protocol_decoder_gm_rke_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
/* One-shot verbatim replay (NO predictor: the encrypted authenticator cannot be
 * regenerated for a bumped counter). Byte-identical to the capture when the .sub
 * was saved after persistence added Prefix/Enc. */
const SubGhzProtocolEncoder subghz_protocol_gm_rke_encoder = {
    .alloc = subghz_protocol_encoder_gm_rke_alloc,
    .free = subghz_protocol_encoder_gm_rke_free,
    .deserialize = subghz_protocol_encoder_gm_rke_deserialize,
    .stop = subghz_protocol_encoder_gm_rke_stop,
    .yield = subghz_protocol_encoder_gm_rke_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_gm_rke_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_gm_rke = {
    .name = GM_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_gm_rke_decoder,
    .encoder = &subghz_protocol_gm_rke_encoder,
};

// ============================================================================
// Field derivation (works both live and after load, from generic.data)
//
// Packed layout of generic.data (59 bits used):
//   [58:27] 32-bit ID   [26:3] 24-bit sequence   [2:0] 3-bit button
// ============================================================================

static void subghz_protocol_gm_rke_check_remote_controller(SubGhzBlockGeneric* generic) {
    generic->btn = (uint8_t)(generic->data & 0x7);
    generic->cnt = (uint32_t)((generic->data >> 3) & 0xFFFFFF);
    generic->serial = (uint32_t)((generic->data >> 27) & 0xFFFFFFFF);
}

static const char* subghz_protocol_gm_rke_button_name(uint8_t btn) {
    switch(btn) {
    case 0x1:
        return "Unlock";
    case 0x2:
        return "Lock";
    case 0x3:
        return "Trunk";
    case 0x4:
        return "Panic";
    default:
        return "?";
    }
}

// ============================================================================
// Frame processing: slice window, validate checksums, emit
// ============================================================================

static void subghz_protocol_gm_rke_process_frame(SubGhzProtocolDecoderGmRke* instance) {
    if(instance->bit_count < GM_RKE_FRAME_BITS) return;

    /* Align to end of collected bits: window = bits[count-113 .. count-2],
     * i.e. 112 bits (14 bytes) starting at (count-113). rtl_433 drops the
     * final collected bit (index count-1). */
    uint16_t off = instance->bit_count - GM_RKE_FRAME_BITS;

    uint8_t bytes[GM_RKE_PAYLOAD_BYTES];
    for(uint16_t i = 0; i < GM_RKE_PAYLOAD_BYTES; i++) {
        uint8_t b = 0;
        for(uint16_t j = 0; j < 8; j++) {
            b = (uint8_t)((b << 1) | (instance->bits[off + i * 8 + j] & 1));
        }
        bytes[i] = b;
    }

    /* Wake-up validation byte. */
    if(bytes[0] != 0xFF) return;

    /* Button checksum: add_nibbles(bytes+2, 1). */
    int button_checksum = (bytes[2] >> 4) + (bytes[2] & 0x0F);
    if(button_checksum == 0 || (button_checksum & 0x0F) != 0) return;

    /* Full checksum: add_bytes(bytes+1, 13). */
    int full_checksum = 0;
    for(uint16_t i = 1; i < GM_RKE_PAYLOAD_BYTES; i++) {
        full_checksum += bytes[i];
    }
    if(full_checksum == 0 || (full_checksum & 0xFF) != 0) return;

    /* Parse fields (matches rtl_433). */
    uint8_t button = bytes[2] & 0x7;
    uint32_t id = ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[4] << 16) |
                  ((uint32_t)bytes[5] << 8) | (uint32_t)bytes[6];
    uint32_t sequence = ((uint32_t)bytes[7] << 16) | ((uint32_t)bytes[8] << 8) | (uint32_t)bytes[9];
    uint32_t encrypted =
        ((uint32_t)bytes[10] << 16) | ((uint32_t)bytes[11] << 8) | (uint32_t)bytes[12];

    /* Pack id/sequence/button into generic.data so serialize/load round-trip. */
    uint64_t packed = ((uint64_t)id << 27) | ((uint64_t)(sequence & 0xFFFFFF) << 3) |
                      (uint64_t)(button & 0x7);

    instance->generic.data = packed;
    instance->generic.data_count_bit = GM_RKE_PAYLOAD_BITS;
    instance->generic.serial = id;
    instance->generic.btn = button;
    instance->generic.cnt = sequence;
    instance->unknown_prefix = bytes[1];
    instance->encrypted = encrypted;

    /* Feed the hash block a meaningful value. */
    instance->decoder.decode_data = packed;
    instance->decoder.decode_count_bit = GM_RKE_PAYLOAD_BITS;

    FURI_LOG_D(
        TAG,
        "ID:%02X%08lX Btn:%X Seq:%06lX",
        bytes[1],
        (unsigned long)id,
        button,
        (unsigned long)sequence);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}







// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_gm_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderGmRke* instance = malloc(sizeof(SubGhzProtocolDecoderGmRke));
    instance->base.protocol = &subghz_protocol_gm_rke;
    instance->generic.protocol_name = subghz_protocol_gm_rke.name;
    return instance;
}

void subghz_protocol_decoder_gm_rke_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_gm_rke_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;
    instance->decoder.parser_step = GmRkeDecoderStepReset;
    instance->bit_count = 0;
}

// ============================================================================
// Decoder: feed
//
// OOK-PPM: each pulse (mark, level=true) is fixed width; the following gap
// (space, level=false) carries one bit. short gap -> 0, long gap -> 1. A gap
// longer than GM_RKE_SPACE_MAX terminates the frame; we then attempt to decode
// the accumulated bits (aligned to the last 113).
// ============================================================================

static void subghz_protocol_gm_rke_add_bit(SubGhzProtocolDecoderGmRke* instance, uint8_t bit) {
    if(instance->bit_count >= GM_RKE_MAX_BITS) {
        /* Slide window down, keep the most recent GM_RKE_KEEP_BITS. */
        uint16_t drop = instance->bit_count - GM_RKE_KEEP_BITS;
        for(uint16_t i = 0; i < GM_RKE_KEEP_BITS; i++) {
            instance->bits[i] = instance->bits[drop + i];
        }
        instance->bit_count = GM_RKE_KEEP_BITS;
    }
    instance->bits[instance->bit_count++] = bit & 1;
}

void subghz_protocol_decoder_gm_rke_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;

    switch(instance->decoder.parser_step) {
    case GmRkeDecoderStepReset:
        /* Wait for a plausible pulse (mark) to begin a frame. */
        if(level && duration >= GM_RKE_MARK_MIN && duration <= GM_RKE_MARK_MAX) {
            instance->bit_count = 0;
            instance->decoder.parser_step = GmRkeDecoderStepData;
        }
        break;

    case GmRkeDecoderStepData:
        if(level) {
            /* Mark: must be a plausible short pulse; otherwise the burst is
             * over (long carrier / noise) -> try to decode, then reset. */
            if(duration < GM_RKE_MARK_MIN || duration > GM_RKE_MARK_MAX) {
                subghz_protocol_gm_rke_process_frame(instance);
                instance->bit_count = 0;
                instance->decoder.parser_step = GmRkeDecoderStepReset;
            }
        } else {
            /* Space: carries the bit, or terminates the frame. */
            if(duration > GM_RKE_SPACE_MAX) {
                subghz_protocol_gm_rke_process_frame(instance);
                instance->bit_count = 0;
                instance->decoder.parser_step = GmRkeDecoderStepReset;
            } else if(duration >= GM_RKE_SPACE_MIN) {
                uint8_t bit = (duration >= GM_RKE_BIT_THRESHOLD) ? 1 : 0;
                subghz_protocol_gm_rke_add_bit(instance, bit);
            } else {
                /* Runt gap: invalid symbol -> abandon this burst. */
                subghz_protocol_gm_rke_process_frame(instance);
                instance->bit_count = 0;
                instance->decoder.parser_step = GmRkeDecoderStepReset;
            }
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize
// ============================================================================

uint8_t subghz_protocol_decoder_gm_rke_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_gm_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        /* Persist the two fields the generic block does not carry, so a one-shot
         * replay can rebuild the exact captured frame (and its checksum). */
        uint8_t prefix = instance->unknown_prefix;
        flipper_format_write_hex(flipper_format, GM_RKE_KEY_PREFIX, &prefix, 1);
        uint8_t enc[3] = {
            (uint8_t)((instance->encrypted >> 16) & 0xFF),
            (uint8_t)((instance->encrypted >> 8) & 0xFF),
            (uint8_t)(instance->encrypted & 0xFF)};
        flipper_format_write_hex(flipper_format, GM_RKE_KEY_ENC, enc, 3);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_gm_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_gm_rke_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        /* Restore the non-generic fields for a faithful get_string display and so
         * the captured values survive load. Absent on legacy files -> left 0. */
        instance->unknown_prefix = 0;
        instance->encrypted = 0;
        uint8_t prefix = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, GM_RKE_KEY_PREFIX, &prefix, 1)) {
            instance->unknown_prefix = prefix;
        }
        uint8_t enc[3] = {0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, GM_RKE_KEY_ENC, enc, 3)) {
            instance->encrypted =
                ((uint32_t)enc[0] << 16) | ((uint32_t)enc[1] << 8) | (uint32_t)enc[2];
        }
    }
    return ret;
}

// ============================================================================
// Decoder: get_string
// ============================================================================

void subghz_protocol_decoder_gm_rke_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderGmRke* instance = context;

    subghz_protocol_gm_rke_check_remote_controller(&instance->generic);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "ID:%02X%08lX\r\n"
        "Btn:%X (%s)\r\n"
        "Seq:%06lX  Enc:%06lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->unknown_prefix,
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        subghz_protocol_gm_rke_button_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->encrypted);
}

// ============================================================================
// Encoder: one-shot verbatim replay (ENABLE_EMULATE_FEATURE)
//
// Rebuilds the exact 14-byte on-air frame from the decoded/loaded fields and
// emits it as OOK-PPM (fixed short mark + short/long gap per bit), the same
// timing the decoder's feed() expects. NO predictor: the encrypted authenticator
// (byte[10..12]) cannot be regenerated for a different counter.
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE

static void* subghz_protocol_encoder_gm_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderGmRke* instance = malloc(sizeof(SubGhzProtocolEncoderGmRke));
    instance->base.protocol = &subghz_protocol_gm_rke;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = GM_RKE_UPLOAD_SIZE;
    instance->encoder.upload = malloc(GM_RKE_UPLOAD_SIZE * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->unknown_prefix = 0;
    instance->encrypted = 0;
    return instance;
}

static void subghz_protocol_encoder_gm_rke_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderGmRke* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/* Build the 14-byte frame, recomputing both checksums so it is byte-correct. */
static void subghz_protocol_encoder_gm_rke_build_frame(
    SubGhzProtocolEncoderGmRke* instance,
    uint8_t bytes[GM_RKE_PAYLOAD_BYTES]) {
    uint8_t button = (uint8_t)(instance->generic.data & 0x7);
    uint32_t id = (uint32_t)((instance->generic.data >> 27) & 0xFFFFFFFF);
    uint32_t sequence = (uint32_t)((instance->generic.data >> 3) & 0xFFFFFF);

    bytes[0] = 0xFF; /* wake-up validation */
    bytes[1] = instance->unknown_prefix;
    /* upper nibble = button checksum so add_nibbles low nibble == 0 */
    uint8_t hi = (uint8_t)((0x10 - button) & 0x0F);
    bytes[2] = (uint8_t)((hi << 4) | (button & 0x7));
    bytes[3] = (uint8_t)((id >> 24) & 0xFF);
    bytes[4] = (uint8_t)((id >> 16) & 0xFF);
    bytes[5] = (uint8_t)((id >> 8) & 0xFF);
    bytes[6] = (uint8_t)(id & 0xFF);
    bytes[7] = (uint8_t)((sequence >> 16) & 0xFF);
    bytes[8] = (uint8_t)((sequence >> 8) & 0xFF);
    bytes[9] = (uint8_t)(sequence & 0xFF);
    bytes[10] = (uint8_t)((instance->encrypted >> 16) & 0xFF);
    bytes[11] = (uint8_t)((instance->encrypted >> 8) & 0xFF);
    bytes[12] = (uint8_t)(instance->encrypted & 0xFF);
    /* full checksum: byte[13] such that add_bytes(bytes+1, 13) low byte == 0 */
    uint32_t sum = 0;
    for(uint16_t i = 1; i < GM_RKE_PAYLOAD_BYTES - 1; i++) {
        sum += bytes[i];
    }
    bytes[13] = (uint8_t)((0x100 - (sum & 0xFF)) & 0xFF);
}

static void subghz_protocol_encoder_gm_rke_add_bit(
    SubGhzProtocolEncoderGmRke* instance,
    size_t* index,
    uint8_t bit) {
    /* fixed short mark, then gap: short = 0, long = 1 */
    instance->encoder.upload[(*index)++] = level_duration_make(true, (uint32_t)GM_RKE_TE_SHORT);
    instance->encoder.upload[(*index)++] =
        level_duration_make(false, (uint32_t)(bit ? GM_RKE_TE_LONG : GM_RKE_TE_SHORT));
}

static bool subghz_protocol_encoder_gm_rke_get_upload(SubGhzProtocolEncoderGmRke* instance) {
    furi_assert(instance);
    uint8_t bytes[GM_RKE_PAYLOAD_BYTES];
    subghz_protocol_encoder_gm_rke_build_frame(instance, bytes);

    size_t index = 0;
    /* wake preamble (short "0" symbols) */
    for(uint16_t k = 0; k < GM_RKE_PREAMBLE_BITS; k++) {
        subghz_protocol_encoder_gm_rke_add_bit(instance, &index, 0);
    }
    /* 112 payload bits, MSB-first per byte */
    for(uint16_t i = 0; i < GM_RKE_PAYLOAD_BYTES; i++) {
        for(int8_t j = 7; j >= 0; j--) {
            subghz_protocol_encoder_gm_rke_add_bit(
                instance, &index, (uint8_t)((bytes[i] >> j) & 1));
        }
    }
    /* one trailing align bit: the decoder end-aligns to the last 113 collected
     * bits and drops the final one, so the payload must sit at [count-113 ..
     * count-2]. preamble(K) + payload(112) + this(1) = K+113 -> slice lands on
     * the payload exactly. */
    subghz_protocol_encoder_gm_rke_add_bit(instance, &index, 0);
    /* terminator: a mark then a long gap (> SPACE_MAX) ends the row */
    instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)GM_RKE_TE_SHORT);
    instance->encoder.upload[index++] = level_duration_make(false, (uint32_t)GM_RKE_TX_GUARD);

    instance->encoder.size_upload = index;
    return index <= (size_t)GM_RKE_UPLOAD_SIZE;
}

static SubGhzProtocolStatus
    subghz_protocol_encoder_gm_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderGmRke* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_gm_rke_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        /* Restore the non-generic frame bytes. If absent (legacy file) they stay
         * 0: the frame still passes both checksums (recomputed) so it round-trips
         * our decoder, but it is not byte-identical to the original capture. */
        instance->unknown_prefix = 0;
        instance->encrypted = 0;
        uint8_t prefix = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, GM_RKE_KEY_PREFIX, &prefix, 1)) {
            instance->unknown_prefix = prefix;
        }
        uint8_t enc[3] = {0, 0, 0};
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_hex(flipper_format, GM_RKE_KEY_ENC, enc, 3)) {
            instance->encrypted =
                ((uint32_t)enc[0] << 16) | ((uint32_t)enc[1] << 8) | (uint32_t)enc[2];
        }

        /* Optional repeat count. */
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        if(!subghz_protocol_encoder_gm_rke_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);
    return ret;
}

static void subghz_protocol_encoder_gm_rke_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderGmRke* instance = context;
    instance->encoder.is_running = false;
}

static LevelDuration subghz_protocol_encoder_gm_rke_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderGmRke* instance = context;

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

#endif /* ENABLE_EMULATE_FEATURE */
