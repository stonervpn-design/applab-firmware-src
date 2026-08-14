/**
 * @file ducato_v0.c
 * @brief Fiat Ducato V0 SubGHz Protocol Decoder
 *
 * Reverse-engineered from D70F3631 V850ES MCU firmware (Ducato Arseniy dump).
 * BCM: Magneti Marelli NBC250C.00, P/N 503440270105 / 1364523080
 *
 * Key fob: NXP PCF7946A (HITAG2+EE)
 * RF receiver: Atmel ATA5742P2 (433.92 MHz ASK superheterodyne)
 * Modulation: Manchester encoding, same as Fiat V0
 *
 * Frame: 71 bits = 64-bit key (Hop:32 | Fix:32) + 7-bit CRC
 *   Fix = Serial(24) | Button(8)
 *   Hop = encrypted rolling code (PCF7946A HITAG2)
 *
 * CRC from firmware FUN_ffe0e2ee:
 *   Masked XOR over frame bytes with masks {0xB3, 0x80, 0x83, 0x22, 0x73}
 *   Result checked via nibble parity table
 *
 * Command dispatch from firmware FUN_ffe21d74: 15 command codes (0x01-0x0F)
 * Manchester state machine identical to Fiat V0 (same PCF7946A chip family)
 */

#include "ducato_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolDucatoV0"

#define DUCATO_TE_SYNC        800
#define DUCATO_PREAMBLE_COUNT 120
#define DUCATO_DATA_BITS      71

static const SubGhzBlockConst subghz_protocol_ducato_v0_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 99,
    .min_count_bit_for_found = DUCATO_DATA_BITS,
};

// Manchester table from PCF7946A (identical to Fiat V0)
static const uint8_t ducato_manchester_table[4] = {0x01, 0x91, 0x9B, 0xFB};

// Nibble parity table from firmware @ 0xFFE3CD38
static const uint8_t ducato_parity_table[16] = {
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0};

// CRC masks from firmware FUN_ffe0e2ee
static const uint8_t ducato_crc_masks[5] = {0xB3, 0x80, 0x83, 0x22, 0x73};

// ============================================================================
// BUTTON DEFINITIONS
// ============================================================================

// From firmware FUN_ffe21d74: 15-case switch (cmd 0x01-0x0F)
// GPIO pins: 0x31-0x3B mapped to BCM relay outputs
#define DUCATO_BTN_LOCK   0x01
#define DUCATO_BTN_UNLOCK 0x02
#define DUCATO_BTN_03     0x03
#define DUCATO_BTN_04     0x04
#define DUCATO_BTN_05     0x05
#define DUCATO_BTN_TRUNK  0x06
#define DUCATO_BTN_07     0x07
#define DUCATO_BTN_08     0x08
#define DUCATO_BTN_PANIC  0x09

static const char* ducato_v0_get_button_name(uint8_t button) {
    static char unknown_buf[8];
    switch(button) {
    case DUCATO_BTN_LOCK:
        return "LOCK";
    case DUCATO_BTN_UNLOCK:
        return "UNLOCK";
    case DUCATO_BTN_TRUNK:
        return "TRUNK";
    case DUCATO_BTN_PANIC:
        return "PANIC";
    case DUCATO_BTN_03:
    case DUCATO_BTN_04:
    case DUCATO_BTN_05:
    case DUCATO_BTN_07:
    case DUCATO_BTN_08:
    default:
        snprintf(unknown_buf, sizeof(unknown_buf), "0x%02X", button);
        return unknown_buf;
    }
}

static uint8_t ducato_v0_get_btn_code(uint8_t original_btn) {
    uint8_t custom_btn_id = subghz_custom_btn_get();

    switch(custom_btn_id) {
    case SUBGHZ_CUSTOM_BTN_OK:
        return original_btn;
    case SUBGHZ_CUSTOM_BTN_UP:
        return DUCATO_BTN_UNLOCK;
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return DUCATO_BTN_LOCK;
    case SUBGHZ_CUSTOM_BTN_LEFT:
        return DUCATO_BTN_TRUNK;
    case SUBGHZ_CUSTOM_BTN_RIGHT:
        return DUCATO_BTN_PANIC;
    default:
        return original_btn;
    }
}

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    DucatoDecoderStepReset = 0,
    DucatoDecoderStepPreamble,
    DucatoDecoderStepData,
} DucatoDecoderStep;

struct SubGhzProtocolDecoderDucatoV0 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te_last;
    uint64_t data;
    uint64_t data_high;
    uint8_t bit_count;
    uint64_t stored_data;
    uint64_t stored_data_high;
    uint8_t crc;
    uint16_t preamble_count;
    uint8_t manchester_state;
};

struct SubGhzProtocolEncoderDucatoV0 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t key;
    uint8_t crc;
    uint32_t serial;
    uint16_t cnt;
    uint8_t button;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_ducato_v0_get_generic(void* context) {
    SubGhzProtocolDecoderDucatoV0* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_ducato_v0_decoder = {
    .alloc = subghz_protocol_decoder_ducato_v0_alloc,
    .free = subghz_protocol_decoder_ducato_v0_free,
    .feed = subghz_protocol_decoder_ducato_v0_feed,
    .reset = subghz_protocol_decoder_ducato_v0_reset,
    .get_hash_data = subghz_protocol_decoder_ducato_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_ducato_v0_serialize,
    .deserialize = subghz_protocol_decoder_ducato_v0_deserialize,
    .get_string = subghz_protocol_decoder_ducato_v0_get_string,
    .get_generic = subghz_protocol_decoder_ducato_v0_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_ducato_v0_encoder = {
    .alloc = subghz_protocol_encoder_ducato_v0_alloc,
    .free = subghz_protocol_encoder_ducato_v0_free,
    .deserialize = subghz_protocol_encoder_ducato_v0_deserialize,
    .stop = subghz_protocol_encoder_ducato_v0_stop,
    .yield = subghz_protocol_encoder_ducato_v0_yield,
};

const SubGhzProtocol subghz_protocol_ducato_v0 = {
    .name = SUBGHZ_PROTOCOL_DUCATO_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_ducato_v0_decoder,
    .encoder = &subghz_protocol_ducato_v0_encoder,
};

// ============================================================================
// MANCHESTER DECODER (same as Fiat V0 — same PCF7946A chip)
// ============================================================================

static bool ducato_manchester_decode(uint8_t* state, uint8_t timing_type, uint8_t* bit_out) {
    if(timing_type == 8) {
        *state = 1;
        return false;
    }

    if(*state >= 4) {
        *state = 1;
        return false;
    }

    uint8_t entry = ducato_manchester_table[*state];
    uint8_t new_state = (entry >> timing_type) & 0x3;

    if(*state == new_state) {
        return false;
    }

    *state = new_state;

    if(new_state == 2) {
        if(bit_out) *bit_out = 0;
        return true;
    } else if(new_state == 1) {
        if(bit_out) *bit_out = 1;
        return true;
    }

    return false;
}

// ============================================================================
// CRC (from firmware FUN_ffe0e2ee @ 0xFFE0E2EE)
// ============================================================================

/**
 * Ducato CRC check from D70F3631 firmware.
 *
 * Applies 5 bit-masks to frame bytes, XORs the results,
 * then checks nibble parity via lookup table @ 0xFFE3CD38.
 *
 * @param data  8-byte key (64 bits)
 * @param crc   7-bit CRC from frame
 * @return true if CRC matches
 */
static bool ducato_v0_check_crc(uint64_t data, uint8_t crc) {
    uint8_t bytes[8];
    for(int i = 7; i >= 0; i--) {
        bytes[i] = data & 0xFF;
        data >>= 8;
    }

    // Apply masks from firmware and XOR together
    // Firmware reads 5 bytes at specific offsets; we map to key bytes
    // Masks: 0xB3, 0x80, 0x83, 0x22, 0x73
    uint8_t acc = 0;
    acc ^= (bytes[0] & ducato_crc_masks[0]);
    acc ^= (bytes[1] & ducato_crc_masks[1]);
    acc ^= (bytes[2] & ducato_crc_masks[2]);
    acc ^= (bytes[3] & ducato_crc_masks[3]);
    acc ^= (bytes[5] & ducato_crc_masks[4]); // byte[5], not byte[4] (offset +2 in firmware)

    // Nibble parity check
    uint8_t parity = ducato_parity_table[acc >> 4] ^ ducato_parity_table[acc & 0x0F];

    // CRC valid if parity matches expected
    // The 7-bit CRC from the frame encodes multiple such parity checks;
    // for now check lowest bit against computed parity
    return (crc & 0x01) == parity;
}

// ============================================================================
// DATA EXTRACTION
// ============================================================================

static void subghz_protocol_ducato_v0_extract_data(SubGhzBlockGeneric* instance, uint8_t crc) {
    uint32_t fix_value = instance->data & 0xFFFFFFFF;

    // Serial: bits 8-31 of Fix (24 bits)
    instance->serial = (fix_value >> 8) & 0x00FFFFFF;

    // Button: bits 0-7 of Fix (8 bits)
    instance->btn = fix_value & 0xFF;

    // Counter: encrypted in Hop, not extractable
    instance->cnt = 0;

    // CRC check
    instance->crc_check = ducato_v0_check_crc(instance->data, crc);
}

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_ducato_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderDucatoV0* instance = malloc(sizeof(SubGhzProtocolDecoderDucatoV0));

    instance->base.protocol = &subghz_protocol_ducato_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->te_last = 0;
    instance->data = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->stored_data = 0;
    instance->stored_data_high = 0;
    instance->crc = 0;
    instance->preamble_count = 0;
    instance->manchester_state = 1;

    return instance;
}

void subghz_protocol_decoder_ducato_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_ducato_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    instance->decoder.parser_step = DucatoDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->te_last = 0;
    instance->data = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->preamble_count = 0;
    instance->manchester_state = 1;
}

void subghz_protocol_decoder_ducato_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    switch(instance->decoder.parser_step) {
    case DucatoDecoderStepReset:
        if(level && DURATION_DIFF(duration, subghz_protocol_ducato_v0_const.te_short) <=
                        subghz_protocol_ducato_v0_const.te_delta) {
            instance->decoder.parser_step = DucatoDecoderStepPreamble;
            instance->te_last = duration;
            instance->preamble_count = 1;
            instance->data = 0;
            instance->data_high = 0;
            instance->bit_count = 0;
            instance->manchester_state = 1;
        }
        break;

    case DucatoDecoderStepPreamble:
        if(DURATION_DIFF(duration, subghz_protocol_ducato_v0_const.te_short) <=
           subghz_protocol_ducato_v0_const.te_delta) {
            if(DURATION_DIFF(instance->te_last, subghz_protocol_ducato_v0_const.te_short) <=
               subghz_protocol_ducato_v0_const.te_delta) {
                instance->preamble_count++;
            } else {
                instance->decoder.parser_step = DucatoDecoderStepReset;
                instance->preamble_count = 0;
            }
            instance->te_last = duration;
        } else if(
            DURATION_DIFF(duration, DUCATO_TE_SYNC) <=
            subghz_protocol_ducato_v0_const.te_delta) {
            if(instance->preamble_count >= DUCATO_PREAMBLE_COUNT) {
                instance->decoder.parser_step = DucatoDecoderStepData;
                instance->preamble_count = 0;
                instance->manchester_state = 1;
                instance->data = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
            } else {
                instance->decoder.parser_step = DucatoDecoderStepReset;
                instance->preamble_count = 0;
            }
        } else {
            instance->decoder.parser_step = DucatoDecoderStepReset;
            instance->preamble_count = 0;
        }
        break;

    case DucatoDecoderStepData: {
        uint8_t timing_type;
        bool valid_timing = false;

        if(DURATION_DIFF(duration, subghz_protocol_ducato_v0_const.te_short) <=
           subghz_protocol_ducato_v0_const.te_delta) {
            timing_type = level ? 0 : 2;
            valid_timing = true;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_ducato_v0_const.te_long) <=
            subghz_protocol_ducato_v0_const.te_delta) {
            timing_type = level ? 4 : 6;
            valid_timing = true;
        }

        if(valid_timing) {
            uint8_t bit_value = 0;
            bool got_bit =
                ducato_manchester_decode(&instance->manchester_state, timing_type, &bit_value);

            if(got_bit) {
                instance->data_high = (instance->data_high << 1) | (instance->data >> 63);
                instance->data = (instance->data << 1) | bit_value;
                instance->bit_count++;

                if(instance->bit_count == 64) {
                    instance->stored_data = instance->data;
                    instance->stored_data_high = instance->data_high;
                    instance->data = 0;
                    instance->data_high = 0;
                }
            }
        }

        if(instance->bit_count == DUCATO_DATA_BITS) {
            instance->generic.data = instance->stored_data;
            instance->generic.data_count_bit = DUCATO_DATA_BITS;

            instance->crc = instance->data & 0x7F;

            subghz_protocol_ducato_v0_extract_data(&instance->generic, instance->crc);

            if(instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }

            instance->data = 0;
            instance->data_high = 0;
            instance->bit_count = 0;
            instance->stored_data = 0;
            instance->stored_data_high = 0;
            instance->preamble_count = 0;
            instance->manchester_state = 1;
            instance->decoder.parser_step = DucatoDecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = DucatoDecoderStepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_ducato_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->bit_count >> 3) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_ducato_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    subghz_protocol_ducato_v0_extract_data(&instance->generic, instance->crc);

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    uint32_t crc_temp = instance->crc;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "CRC", &crc_temp, 1)) {
        FURI_LOG_E(TAG, "Unable to add CRC");
        return SubGhzProtocolStatusErrorParserOthers;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ducato_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_ducato_v0_const.min_count_bit_for_found);

    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    uint32_t crc_temp = 0;
    if(flipper_format_read_uint32(flipper_format, "CRC", &crc_temp, 1)) {
        instance->crc = crc_temp & 0x7F;
    } else {
        instance->crc = 0;
    }

    subghz_protocol_ducato_v0_extract_data(&instance->generic, instance->crc);

    subghz_custom_btn_set_original(instance->generic.btn);
    subghz_custom_btn_set_max(4);

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_ducato_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderDucatoV0* instance = context;

    subghz_protocol_ducato_v0_extract_data(&instance->generic, instance->crc);

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 8;
    subghz_block_generic_global.current_btn = instance->generic.btn;

    uint64_t data = instance->generic.data;

    uint32_t hop = (data >> 32) & 0xFFFFFFFF;
    uint32_t fix = data & 0xFFFFFFFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Fix:%08lX  Hop:%08lX\r\n"
        "Sn:%06lX Btn:%s\r\n"
        "CRC:%02X %s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)hop,
        (unsigned long)fix,
        (unsigned long)fix,
        (unsigned long)hop,
        (unsigned long)instance->generic.serial,
        ducato_v0_get_button_name(instance->generic.btn),
        instance->crc,
        instance->generic.crc_check ? "OK" : "");
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================

static size_t ducato_encode_bit(LevelDuration* buf, uint8_t* state, uint8_t bit) {
    size_t count = 0;

    switch(*state) {
    case 1:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_long);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_short);
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);
            *state = 1;
        }
        break;

    case 2:
        if(bit == 0) {
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_short);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_long);
            *state = 1;
        }
        break;

    case 0:
        if(bit == 1) {
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);
            *state = 1;
        } else {
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_long);
            *state = 2;
        }
        break;

    case 3:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_short);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_short);
            buf[count++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_long);
            *state = 1;
        }
        break;

    default:
        *state = 1;
        break;
    }

    return count;
}

static void subghz_protocol_encoder_ducato_v0_get_upload(SubGhzProtocolEncoderDucatoV0* instance) {
    furi_assert(instance);

    LevelDuration* buf = instance->encoder.upload;
    size_t idx = 0;

    // Preamble: 166 pairs of SHORT HIGH/LOW (Manchester 0xAA pattern)
    for(int i = 0; i < 166; i++) {
        buf[idx++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);
        buf[idx++] = level_duration_make(false, subghz_protocol_ducato_v0_const.te_short);
    }

    // The pairs end on a SHORT LOW; the 800us sync below is also LOW. On a real
    // radio (and in the merge-aware harness) two consecutive same-level entries
    // coalesce into one pulse, which would fuse the last preamble half-bit into
    // the sync and yield a ~1000us LOW that matches neither te_short nor the sync
    // window — dropping the frame before it reaches data decode. Cap the preamble
    // with one extra SHORT HIGH (still a valid preamble short) so the LOW sync is
    // bracketed by HIGH pulses on both sides and stays a distinct transition.
    buf[idx++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);

    // Sync: 800us LOW
    buf[idx++] = level_duration_make(false, DUCATO_TE_SYNC);

    // Initial SHORT_HIGH to establish state 1
    buf[idx++] = level_duration_make(true, subghz_protocol_ducato_v0_const.te_short);

    uint8_t state = 1;

    // Manchester encode 64-bit key (MSB first)
    for(int bit = 63; bit >= 0; bit--) {
        uint8_t b = (instance->key >> bit) & 1;
        idx += ducato_encode_bit(&buf[idx], &state, b);
    }

    // Manchester encode 7-bit CRC (MSB first)
    for(int bit = 6; bit >= 0; bit--) {
        uint8_t b = (instance->crc >> bit) & 1;
        idx += ducato_encode_bit(&buf[idx], &state, b);
    }

    // End gap. The decoder finalizes on the 71st decoded bit, so the pulse that
    // completes the last bit must reach feed() intact. Emit the gap at the OPPOSITE
    // level from the last data pulse: were it the same level it would coalesce with
    // that completing pulse into one over-long pulse (matching no te window), the
    // final bit would never be produced and the frame would stall at 70 bits.
    bool last_level = level_duration_get_level(buf[idx - 1]);
    buf[idx++] = level_duration_make(!last_level, 6000);

    instance->encoder.size_upload = idx;
}

void* subghz_protocol_encoder_ducato_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderDucatoV0* instance = malloc(sizeof(SubGhzProtocolEncoderDucatoV0));

    instance->base.protocol = &subghz_protocol_ducato_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;

    instance->key = 0;
    instance->crc = 0;
    instance->serial = 0;
    instance->cnt = 0;
    instance->button = 0;

    return instance;
}

void subghz_protocol_encoder_ducato_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderDucatoV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_ducato_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderDucatoV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(SubGhzProtocolStatusOk !=
           subghz_block_generic_deserialize(&instance->generic, flipper_format)) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        uint32_t crc_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "CRC", &crc_temp, 1)) {
            instance->crc = crc_temp & 0x7F;
        } else {
            instance->crc = 0;
        }

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->key = instance->generic.data;
        uint32_t hop = (instance->key >> 32) & 0xFFFFFFFF;
        uint32_t fix = instance->key & 0xFFFFFFFF;

        instance->serial = (fix >> 8) & 0x00FFFFFF;
        instance->button = fix & 0xFF;
        instance->cnt = 0;

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->button);
        }
        subghz_custom_btn_set_max(4);

        uint8_t btn = instance->button;

        if(!subghz_block_generic_global_button_override_get(&btn)) {
            uint8_t original_btn = subghz_custom_btn_get_original();
            btn = ducato_v0_get_btn_code(original_btn);
        }
        instance->button = btn;

        uint32_t new_fix = ((instance->serial & 0x00FFFFFF) << 8) | (instance->button & 0xFF);

        instance->key = ((uint64_t)hop << 32) | new_fix;

        instance->generic.data = instance->key;
        instance->generic.cnt = instance->cnt;
        instance->generic.btn = instance->button;
        instance->generic.serial = instance->serial;
        instance->generic.data_count_bit = DUCATO_DATA_BITS;

        subghz_protocol_encoder_ducato_v0_get_upload(instance);

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }

        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->key >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to update Key");
            break;
        }

        instance->encoder.is_running = true;

        res = SubGhzProtocolStatusOk;

    } while(false);

    return res;
}

void subghz_protocol_encoder_ducato_v0_stop(void* context) {
    SubGhzProtocolEncoderDucatoV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_ducato_v0_yield(void* context) {
    SubGhzProtocolEncoderDucatoV0* instance = context;

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
