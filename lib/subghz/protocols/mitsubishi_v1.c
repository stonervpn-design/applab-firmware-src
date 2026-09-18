/**
 * Mitsubishi SubGHz Protocol V1 — 2-FSK Manchester (KeyFob Gen mit Sync 0x1E4C11A7)
 *
 * Physical: 2-FSK @ 433.92 MHz
 *   - Preset FuriHalSubGhzPreset2FSKDev476Async
 *   Manchester (G.E. Thomas), te_short=200us, te_long=400us.
 *   Preamble: viele alternierende SHORT-Pulse, danach Sync-Wort.
 *
 * Unterscheidet sich von Mitsubishi V0 (Sync 0x8930, Btn/Cnt je 8 bit):
 * andere Keyfob-Generation, 32-bit Sync, Button-Nibble + 12-bit Counter.
 *
 * Data: 96 bits (12 bytes)
 *   [Sync:32] [Fix:8] [Button:4] [Counter:10] [Hop:42]
 *   Bits  0-31 : 0x1E4C11A7  Sync-Wort (konstant)
 *   Bits 32-39 : 0xE0        Fix-Byte (konstant; bei nur 1 Fob nicht von Serial trennbar)
 *   Bits 40-43 : Button-Nibble  2=Lock, 8=Unlock, 4=Trunk
 *   Bits 44-53 : Counter (10 bit), global ueber alle Buttons, +1/Druck, Wrap bei 0x3FF
 *   Bits 54-95 : Encrypted rolling code (42 bit)
 *
 * Validierung (6 Aufnahmen, 1 Keyfob, 111 Tastendruecke):
 *   - Sync 0x1E4C11A7 in allen Frames konstant
 *   - Button-Nibble konstant pro Datei (Lock/Unlock/Trunk)
 *   - Counter zaehlt sauber +1/Druck, global ueber alle 6 Dateien (103/110 Schritte
 *     exakt +1; die Ausreisser sind nicht aufgezeichnete Tastendruecke zwischen den
 *     Aufnahmen, dort +2/+3). Brute-Force ueber alle Bit-Offsets bestaetigt [44:53)
 *     als einzige Position mit sauberem +1.
 *   - Hop wechselt komplett pro Druck (verschluesselt) -> Dev-Protokoll, kein Encoder
 *
 * Crypto-Analyse Hop (42 bit [54:96), 111 Frames, analyseCrypto.py + GF(2)/BM):
 *   - Avalanche der variablen Bits ~52% (ideal), bijektiv (keine echten Kollisionen)
 *   - Berlekamp-Massey LC = n/2 -> maximale lineare Komplexitaet (Cipher-Grade)
 *   - KEINE lineare Output-Schicht ueber dem Counter (anders als Mazda V3): GF(2)-Fit
 *     findet 0 echte Counter-Leaks -> nicht brechbar ohne Key/mehr Fobs
 *   - XOR(n+1,n) immer distinct -> kein XOR; Honda-KeeLoq-Keys: kein Match
 *   - Output ~38 bit echte Cipher + ~4 Struktur-Bits (frame 54=1,59=0,86=1,95=1)
 *   - Breite passt NICHT zu KeeLoq(32)/Hitag2(48); Mode (counter-getrieben) != DST40
 *     -> wahrscheinlich proprietaerer 40-bit Block/NLFSR-Cipher (RKE-ASIC) o. AES-trunc
 */

#include "mitsubishi_v1.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolMitsubishiV1"

#define MITSUBISHI_V1_DATA_BITS    96
#define MITSUBISHI_V1_ALL_BITS_MAX 200

static const SubGhzBlockConst subghz_protocol_mitsubishi_v1_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = 88,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    MitsubishiV1DecoderStepReset = 0,
    MitsubishiV1DecoderStepCollect,
} MitsubishiV1DecoderStep;

struct SubGhzProtocolDecoderMitsubishiV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    // Stateless Manchester (Ansatz C): half-bit toggle
    bool half;

    uint8_t all_bits[MITSUBISHI_V1_ALL_BITS_MAX];
    uint16_t all_bit_count;

    uint64_t hop_40;
};

struct SubGhzProtocolEncoderMitsubishiV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// SYNC SEARCH — 0x1E4C11A7 = 00011110 01001100 00010001 10100111
// ============================================================================

static const uint8_t mitsubishi_v1_sync_bits[32] = {
    0,0,0,1, 1,1,1,0, 0,1,0,0, 1,1,0,0,
    0,0,0,1, 0,0,0,1, 1,0,1,0, 0,1,1,1};

static int mitsubishi_v1_find_sync(const uint8_t* bits, int bit_count) {
    int need = 56; // 56 bits nach Sync (fix+btn+cnt+hop_anteil)
    int search_end = bit_count - 32 - need;

    for(int i = 0; i <= search_end; i++) {
        bool match = true;
        for(int j = 0; j < 32; j++) {
            if(bits[i + j] != mitsubishi_v1_sync_bits[j]) {
                match = false;
                break;
            }
        }
        if(match) return i;
    }
    return -1;
}

// ============================================================================
// FRAME PROCESSING
// ============================================================================

static void mitsubishi_v1_process_frame(SubGhzProtocolDecoderMitsubishiV1* instance) {
    if(instance->all_bit_count < 88) return;

    int sync_pos = mitsubishi_v1_find_sync(instance->all_bits, instance->all_bit_count);
    if(sync_pos < 0) {
        FURI_LOG_D(TAG, "No sync in %d bits", instance->all_bit_count);
        return;
    }

    int data_start = sync_pos;
    int data_len = instance->all_bit_count - data_start;
    if(data_len > MITSUBISHI_V1_DATA_BITS) data_len = MITSUBISHI_V1_DATA_BITS;
    if(data_len < 88) return;

    // Erste 64 Bits in generic.data
    instance->generic.data = 0;
    for(int i = 0; i < 64 && i < data_len; i++) {
        instance->generic.data =
            (instance->generic.data << 1) | instance->all_bits[data_start + i];
    }

    // Hop = 42 bit ab Bit 54 (Rolling Code; enthaelt die 2 Bits [54:56) die NICHT
    // zum Counter gehoeren)
    instance->hop_40 = 0;
    int hop_bits = 0;
    for(int i = 54; i < data_len && i < 54 + 42; i++) {
        instance->hop_40 = (instance->hop_40 << 1) | instance->all_bits[data_start + i];
        hop_bits++;
    }
    if(hop_bits < 42) instance->hop_40 <<= (42 - hop_bits);

    instance->generic.data_count_bit = data_len;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = data_len;

    // Felder aus generic.data (= Bits 0..63):
    //   [0:31] Sync  [32:39] Fix  [40:43] Btn  [44:53] Cnt(10)  [54:63] Hop_hi10
    instance->generic.btn = (uint8_t)((instance->generic.data >> 20) & 0x0F);
    instance->generic.cnt = (uint32_t)((instance->generic.data >> 10) & 0x3FF);
    instance->generic.serial = (uint32_t)((instance->generic.data >> 24) & 0xFF); // Fix-Byte 0xE0

    FURI_LOG_I(
        TAG,
        "Decoded %d bits: Btn=%01X Cnt=%03lX Hop=%011llX",
        data_len,
        instance->generic.btn,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)instance->hop_40);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// PROTOCOL VTABLES
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_mitsubishi_v1_get_generic(void* context) {
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_mitsubishi_v1_decoder = {
    .alloc = subghz_protocol_decoder_mitsubishi_v1_alloc,
    .free = subghz_protocol_decoder_mitsubishi_v1_free,
    .feed = subghz_protocol_decoder_mitsubishi_v1_feed,
    .reset = subghz_protocol_decoder_mitsubishi_v1_reset,
    .get_hash_data = subghz_protocol_decoder_mitsubishi_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_mitsubishi_v1_serialize,
    .deserialize = subghz_protocol_decoder_mitsubishi_v1_deserialize,
    .get_string = subghz_protocol_decoder_mitsubishi_v1_get_string,
    .get_generic = subghz_protocol_decoder_mitsubishi_v1_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_mitsubishi_v1_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_mitsubishi_v1 = {
    .name = SUBGHZ_PROTOCOL_MITSUBISHI_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_mitsubishi_v1_decoder,
    .encoder = &subghz_protocol_mitsubishi_v1_encoder,
};

// ============================================================================
// DECODER: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_mitsubishi_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMitsubishiV1* instance =
        malloc(sizeof(SubGhzProtocolDecoderMitsubishiV1));
    instance->base.protocol = &subghz_protocol_mitsubishi_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_mitsubishi_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    free(instance);
}

void subghz_protocol_decoder_mitsubishi_v1_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    instance->decoder.parser_step = MitsubishiV1DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->all_bit_count = 0;
    instance->half = false;
    instance->generic.data_count_bit = 0;
}

// ============================================================================
// DECODER: feed — Stateless Manchester (Ansatz C)
//
//   LONG          -> 1 Bit, Wert = Level
//   SHORT (2.)    -> 1 Bit, Wert = Level des zweiten SHORT
//   SHORT (1.)    -> nur Halbbit-Marker, kein Bit
//   Glitch (<100) -> ignorieren
//   Gap (>te_long+te_delta) -> Frame-Ende: process + reset
// ============================================================================

void subghz_protocol_decoder_mitsubishi_v1_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;

    uint32_t te_s = subghz_protocol_mitsubishi_v1_const.te_short;
    uint32_t te_l = subghz_protocol_mitsubishi_v1_const.te_long;
    uint32_t te_d = subghz_protocol_mitsubishi_v1_const.te_delta;

    switch(instance->decoder.parser_step) {
    case MitsubishiV1DecoderStepReset:
        // Start sammeln sobald ein gueltiger SHORT/LONG ankommt
        if(duration >= te_s - te_d && duration <= te_l + te_d) {
            instance->decoder.parser_step = MitsubishiV1DecoderStepCollect;
            instance->all_bit_count = 0;
            instance->half = false;
            // diesen ersten Puls direkt mitverarbeiten (fall-through)
        } else {
            break;
        }
        /* fall through */

    case MitsubishiV1DecoderStepCollect: {
        if(instance->all_bit_count >= MITSUBISHI_V1_ALL_BITS_MAX - 1) {
            mitsubishi_v1_process_frame(instance);
            instance->decoder.parser_step = MitsubishiV1DecoderStepReset;
            break;
        }

        if(duration < (te_s / 2)) {
            // Glitch: ignorieren, Zustand erhalten
            break;
        }

        bool is_long = duration > (te_s + te_l) / 2 && duration <= te_l + te_d;
        bool is_short = duration >= te_s - te_d && duration <= (te_s + te_l) / 2;

        if(duration > te_l + te_d) {
            // Gap / Frame-Ende
            mitsubishi_v1_process_frame(instance);
            instance->decoder.parser_step = MitsubishiV1DecoderStepReset;
            break;
        }

        if(is_long) {
            instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
            instance->half = false;
        } else if(is_short) {
            if(instance->half) {
                instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                instance->half = false;
            } else {
                instance->half = true;
            }
        }
    } break;

    default:
        instance->decoder.parser_step = MitsubishiV1DecoderStepReset;
        break;
    }
}

// ============================================================================
// DECODER: hash / serialize / deserialize
// ============================================================================

uint8_t subghz_protocol_decoder_mitsubishi_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_mitsubishi_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t hop_hi = (uint32_t)(instance->hop_40 >> 16); // 26 bit
        uint32_t hop_lo = (uint32_t)(instance->hop_40 & 0xFFFF); // 16 bit
        flipper_format_write_uint32(flipper_format, "Hop_Hi", &hop_hi, 1);
        flipper_format_write_uint32(flipper_format, "Hop_Lo", &hop_lo, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mitsubishi_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_mitsubishi_v1_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t hop_hi = 0, hop_lo = 0;
        flipper_format_read_uint32(flipper_format, "Hop_Hi", &hop_hi, 1);
        flipper_format_read_uint32(flipper_format, "Hop_Lo", &hop_lo, 1);
        instance->hop_40 = ((uint64_t)hop_hi << 16) | hop_lo;
    }
    return ret;
}

// ============================================================================
// DECODER: get_string
// ============================================================================

static const char* mitsubishi_v1_btn_name(uint8_t btn) {
    switch(btn & 0x0F) {
    case 0x2: return "Lock";
    case 0x8: return "Unlock";
    case 0x4: return "Trunk";
    default: return "??";
    }
}

void subghz_protocol_decoder_mitsubishi_v1_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderMitsubishiV1* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "Fix:1E4C11A7E0 Btn:%01X [%s]\r\n"
        "Cnt:%03lX Hop:%011llX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)instance->generic.data,
        instance->generic.btn,
        mitsubishi_v1_btn_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt,
        (unsigned long long)instance->hop_40);
}
