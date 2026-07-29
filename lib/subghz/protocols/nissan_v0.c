/**
 * Nissan RKE SubGHz Protocol — OOK Manchester (slow timing)
 *
 * Physical: OOK/AM @ 433.92 MHz, FuriHalSubGhzPresetOok650Async
 * Manchester: te_short (half-bit) = 500us, te_long (full bit) = 1000us
 *   -> data rate ~1 kbit/s (notably slower than most vehicle remotes)
 *
 * Frame: 64 bits, repeated ~6x per button press, fully rolling code.
 *   bit  0.. 5 : Sync  = 110001        (constant, top 6 bits of byte0 = 0xC4..0xC7)
 *   bit  6.. 7 : rolling
 *   bit  8..47 : rolling (bytes 1-5)
 *   bit 48..53 : Marker = 000010        (constant, byte6 high part = 0x08..0x0B)
 *   bit 54..55 : rolling
 *   bit 56..63 : rolling (byte7, likely checksum)
 *
 * The 56-bit payload is fully encrypted/rolling — no cleartext serial, button
 * or counter is separable, so only Key/Hop are reported (decode-only dev).
 *
 * Captures show two sync-aligned byte0 families (0xC4..C7) and two byte6 values
 * (0x08..0x0B); these low bits roll and are not a stable type/button field.
 */

#include "nissan_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolNissanV0"

#define NISSAN_V0_FRAME_BITS   64
#define NISSAN_V0_TE_SHORT     500  // half-bit
#define NISSAN_V0_TE_LONG      1000 // full bit
#define NISSAN_V0_HALFBIT_CAP  380  // process buffer when this many half-bits collected
#define NISSAN_V0_BURSTS       3    // encoder: frame repeats

// Sync word (6 bits) at frame offset 0
static const uint8_t nissan_v0_sync[6] = {1, 1, 0, 0, 0, 1};
// Marker (6 bits) at frame offset 48 — strengthens the lock (sync+marker = 12 bits)
static const uint8_t nissan_v0_marker[6] = {0, 0, 0, 0, 1, 0};

static const SubGhzBlockConst subghz_protocol_nissan_v0_const __attribute__((unused)) = {
    .te_short = NISSAN_V0_TE_SHORT,
    .te_long = NISSAN_V0_TE_LONG,
    .te_delta = 200,
    .min_count_bit_for_found = NISSAN_V0_FRAME_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SubGhzProtocolDecoderNissanV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t halfbits[512]; // accumulated half-bit level samples
    uint16_t hb_count;
    uint64_t last_data; // de-dup repeated frames within one press
};

struct SubGhzProtocolEncoderNissanV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_nissan_v0_get_generic(void* context) {
    SubGhzProtocolDecoderNissanV0* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_nissan_v0_decoder = {
    .alloc = subghz_protocol_decoder_nissan_v0_alloc,
    .free = subghz_protocol_decoder_nissan_v0_free,
    .feed = subghz_protocol_decoder_nissan_v0_feed,
    .reset = subghz_protocol_decoder_nissan_v0_reset,
    .get_hash_data = subghz_protocol_decoder_nissan_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_nissan_v0_serialize,
    .deserialize = subghz_protocol_decoder_nissan_v0_deserialize,
    .get_string = subghz_protocol_decoder_nissan_v0_get_string,
    .get_generic = subghz_protocol_decoder_nissan_v0_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_nissan_v0_encoder = {
    .alloc = subghz_protocol_encoder_nissan_v0_alloc,
    .free = subghz_protocol_encoder_nissan_v0_free,
    .deserialize = subghz_protocol_encoder_nissan_v0_deserialize,
    .stop = subghz_protocol_encoder_nissan_v0_stop,
    .yield = subghz_protocol_encoder_nissan_v0_yield,
};

const SubGhzProtocol subghz_protocol_nissan_v0 = {
    .name = SUBGHZ_PROTOCOL_NISSAN_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_nissan_v0_decoder,
    .encoder = &subghz_protocol_nissan_v0_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_nissan_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderNissanV0* instance = malloc(sizeof(SubGhzProtocolDecoderNissanV0));
    instance->base.protocol = &subghz_protocol_nissan_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->hb_count = 0;
    instance->last_data = 0;
    return instance;
}

void subghz_protocol_decoder_nissan_v0_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_nissan_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->hb_count = 0;
    instance->last_data = 0;
}

/**
 * Manchester-decode the accumulated half-bit buffer, search for the sync word
 * and (at offset 48) the marker, then extract the 64-bit frame.
 */
static void nissan_v0_process(SubGhzProtocolDecoderNissanV0* instance) {
    if(instance->hb_count < 140) {
        instance->hb_count = 0;
        return;
    }

    // Stateless Manchester: pair differing half-bits, slip on equal (re-align).
    // (0,1) low->high = bit 1 ; (1,0) high->low = bit 0.
    uint8_t bits[300];
    int bc = 0;
    int i = 0;
    while(i + 1 < instance->hb_count && bc < (int)sizeof(bits)) {
        uint8_t a = instance->halfbits[i];
        uint8_t b = instance->halfbits[i + 1];
        if(a == b) {
            i++;
            continue;
        }
        bits[bc++] = (a == 0 && b == 1) ? 1 : 0;
        i += 2;
    }

    // Search for sync (6 bits) + marker (6 bits @ +48), need 64 bits total.
    int found = -1;
    for(int p = 0; p + NISSAN_V0_FRAME_BITS <= bc; p++) {
        bool ok = true;
        for(int j = 0; j < 6; j++) {
            if(bits[p + j] != nissan_v0_sync[j]) {
                ok = false;
                break;
            }
        }
        if(!ok) continue;
        for(int j = 0; j < 6; j++) {
            if(bits[p + 48 + j] != nissan_v0_marker[j]) {
                ok = false;
                break;
            }
        }
        if(ok) {
            found = p;
            break;
        }
    }

    instance->hb_count = 0;
    if(found < 0) {
        FURI_LOG_D(TAG, "No sync/marker in %d bits", bc);
        return;
    }

    uint64_t data = 0;
    for(int j = 0; j < NISSAN_V0_FRAME_BITS; j++) {
        data = (data << 1) | bits[found + j];
    }

    if(data == instance->last_data) return; // suppress repeats of same press
    instance->last_data = data;

    instance->generic.data = data;
    instance->generic.data_count_bit = NISSAN_V0_FRAME_BITS;
    instance->generic.serial = 0; // no cleartext serial
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = NISSAN_V0_FRAME_BITS;

    FURI_LOG_I(TAG, "Decoded frame %08lX%08lX", (unsigned long)(data >> 32),
               (unsigned long)(data & 0xFFFFFFFF));

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_nissan_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;

    // Classify duration as a number of half-bit (500us) units; off-grid -> flush.
    int nh;
    if(duration >= 320 && duration <= 700) {
        nh = 1;
    } else if(duration >= 800 && duration <= 1230) {
        nh = 2;
    } else if(duration >= 1300 && duration <= 1730) {
        nh = 3;
    } else if(duration >= 1800 && duration <= 2350) {
        nh = 4;
    } else {
        // gap / noise — process whatever we have and restart
        nissan_v0_process(instance);
        return;
    }

    uint8_t v = level ? 1 : 0;
    for(int k = 0; k < nh; k++) {
        if(instance->hb_count < sizeof(instance->halfbits)) {
            instance->halfbits[instance->hb_count++] = v;
        }
    }

    if(instance->hb_count >= NISSAN_V0_HALFBIT_CAP) {
        nissan_v0_process(instance);
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_nissan_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_nissan_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Nissan V0 - Hop:%08lX%08lX",
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_nissan_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    instance->generic.protocol_name = instance->base.protocol->name;
    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_nissan_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderNissanV0* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Hop:%08lX%08lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)key_hi,
        (unsigned long)key_lo);
}

// ============================================================================
// ENCODER (replay — emits the stored 64-bit frame as Manchester)
// ============================================================================

void* subghz_protocol_encoder_nissan_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderNissanV0* instance = malloc(sizeof(SubGhzProtocolEncoderNissanV0));
    instance->base.protocol = &subghz_protocol_nissan_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = NISSAN_V0_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_nissan_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderNissanV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one frame: [preamble] [64 Manchester bits] [end gap].
 * Bits are emitted as half-bit pairs (bit1=low,high ; bit0=high,low) and then
 * run-length merged so consecutive equal levels become one LONG pulse — exactly
 * what the decoder expects.
 */
static void nissan_v0_build_upload(SubGhzProtocolEncoderNissanV0* instance) {
    furi_assert(instance);

    uint8_t hb[32 + NISSAN_V0_FRAME_BITS * 2];
    int hc = 0;

    // Preamble: 24 alternating half-bits (decodes to constant, contains no sync)
    for(int p = 0; p < 24; p++) {
        hb[hc++] = (p & 1) ? 0 : 1;
    }

    // Data bits, MSB first
    for(int b = 0; b < NISSAN_V0_FRAME_BITS; b++) {
        uint8_t bit = (instance->generic.data >> (NISSAN_V0_FRAME_BITS - 1 - b)) & 1;
        if(bit) {
            hb[hc++] = 0;
            hb[hc++] = 1; // bit 1 = low,high
        } else {
            hb[hc++] = 1;
            hb[hc++] = 0; // bit 0 = high,low
        }
    }

    // Run-length merge into LevelDuration entries
    size_t idx = 0;
    int j = 0;
    while(j < hc) {
        uint8_t lvl = hb[j];
        int cnt = 1;
        while(j + cnt < hc && hb[j + cnt] == lvl) cnt++;
        instance->encoder.upload[idx++] =
            level_duration_make(lvl ? true : false, (uint32_t)cnt * NISSAN_V0_TE_SHORT);
        j += cnt;
    }

    // End gap
    instance->encoder.upload[idx++] = level_duration_make(false, 4000);
    instance->encoder.size_upload = idx;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_nissan_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderNissanV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        nissan_v0_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_nissan_v0_stop(void* context) {
    SubGhzProtocolEncoderNissanV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_nissan_v0_yield(void* context) {
    SubGhzProtocolEncoderNissanV0* instance = context;

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
