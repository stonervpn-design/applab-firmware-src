/**
 * @file honda_v5.c
 * @brief Honda V5 SubGHz Protocol - Decoder and Encoder
 *
 * Rolling code protocol used by Honda vehicle key fobs.
 * Uses Manchester encoding with custom CC1101 preset at 433.657 MHz.
 *
 * Signal: 433.657 MHz, Custom CC1101 preset (OOK-like), Manchester encoding
 * Timing: te_short=63us, te_long=126us, te_delta=20
 *
 * Transmission per keypress: 2 frames
 *   Frame 1 (TX):  [PREAMBLE ~360 shorts][Manchester 135b, byte3=0x08]
 *   Frame 2 (RTX): [PREAMBLE ~360 shorts][Manchester 135b, byte3=0x0A]
 *   RTX tail = TX tail XOR 0xC4
 *
 * Data format (135 bits, MSB-padded to 17 bytes):
 *   Bytes 0-2:   Header (0x6C0F62, constant)
 *   Byte 3:      Retransmit flag (0x08=TX, 0x0A=RTX)
 *   Bytes 4-7:   Serial (32-bit, constant per remote)
 *   Byte 8:      Button (0x21=Lock, 0x22=Unlock)
 *   Byte 9:      Padding (0x00)
 *   Bytes 10-11: Counter (16-bit, per-button independent counters)
 *   Bytes 12-15: Rolling code (32-bit)
 *   Byte 16:     Tail/CRC (8-bit)
 */

#include "honda_v5.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolHondaV5"

// ============================================================================
// PROTOCOL TIMING
// ============================================================================

static const SubGhzBlockConst subghz_protocol_honda_v5_const = {
    .te_short = 63,
    .te_long = 126,
    .te_delta = 20,
    .min_count_bit_for_found = 130, // slightly below 135 for tolerance
};

#define HONDA_V5_DATA_BITS       135
#define HONDA_V5_DATA_BYTES      17
#define HONDA_V5_ALT_DATA_BITS   168 // alternate variant: 21 bytes, 8-bit aligned
#define HONDA_V5_ALT_DATA_BYTES  21
#define HONDA_V5_ALT_HEADER      0x6428 // 2-byte header (8-bit aligned)
#define HONDA_V5_MAX_BITS        400 // max bits in accumulator (preamble ~180 + data 168)
#define HONDA_V5_PREAMBLE_SHORTS 360 // typical preamble pulse count
#define HONDA_V5_HEADER_VALUE    0x6C0F62
#define HONDA_V5_HEADER_ALT      0x3214   // 2-byte prefix for alternate models (e.g. Civic 2008)
#define HONDA_V5_RETRANSMIT_TX   0x08
#define HONDA_V5_RETRANSMIT_RTX  0x0A
#define HONDA_V5_TAIL_XOR        0xC4

// ============================================================================
// MANCHESTER TABLE & DECODE (same LUT style as fiat)
// ============================================================================

static const uint8_t honda_v5_manchester_table[4] = {0x01, 0x91, 0x9B, 0xFB};

static bool honda_v5_manchester_decode(uint8_t* state, uint8_t timing_type, uint8_t* bit_out) {
    if(timing_type == 8) {
        *state = 1;
        return false;
    }
    if(*state >= 4) {
        *state = 1;
        return false;
    }

    uint8_t entry = honda_v5_manchester_table[*state];
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
// DATA STRUCTURES
// ============================================================================

typedef enum {
    HondaV5DecoderStepReset = 0,
    HondaV5DecoderStepData = 1,
} HondaV5DecoderStep;

struct SubGhzProtocolDecoderHondaV5 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t manchester_state;
    uint8_t raw_data[22]; // decoded bytes (17 for V5, 21 for alt variant + padding)
    uint16_t bit_count;
    uint8_t bits[HONDA_V5_MAX_BITS]; // individual bits for Manchester decode

    // Parsed fields
    uint32_t header; // bytes 0-2 (24-bit, should be 0x6C0F62)
    uint8_t retransmit; // byte 3 (0x08 or 0x0A)
    uint32_t serial; // bytes 4-7
    uint8_t button; // byte 8
    uint8_t padding; // byte 9
    uint16_t counter; // bytes 10-11
    uint32_t rolling; // bytes 12-15
    uint8_t tail; // byte 16
    bool crc_ok; // crypto CRC verification result
    bool is_alt; // true if alternate 168-bit variant
};

struct SubGhzProtocolEncoderHondaV5 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t header;
    uint32_t serial;
    uint8_t button;
    uint16_t counter;
    uint32_t rolling;
    uint8_t tail;
    uint8_t retransmit;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v5_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV5* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v5_decoder = {
    .alloc = subghz_protocol_decoder_honda_v5_alloc,
    .free = subghz_protocol_decoder_honda_v5_free,

    .feed = subghz_protocol_decoder_honda_v5_feed,
    .reset = subghz_protocol_decoder_honda_v5_reset,

    .get_hash_data = subghz_protocol_decoder_honda_v5_get_hash_data,
    .serialize = subghz_protocol_decoder_honda_v5_serialize,
    .deserialize = subghz_protocol_decoder_honda_v5_deserialize,
    .get_string = subghz_protocol_decoder_honda_v5_get_string,
    .get_generic = subghz_protocol_decoder_honda_v5_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v5_encoder = {
    .alloc = subghz_protocol_encoder_honda_v5_alloc,
    .free = subghz_protocol_encoder_honda_v5_free,

    .deserialize = subghz_protocol_encoder_honda_v5_deserialize,
    .stop = subghz_protocol_encoder_honda_v5_stop,
    .yield = subghz_protocol_encoder_honda_v5_yield,
};

const SubGhzProtocol subghz_protocol_honda_v5 = {
    .name = SUBGHZ_PROTOCOL_HONDA_V5_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_honda_v5_decoder,
    .encoder = &subghz_protocol_honda_v5_encoder,
};

// ============================================================================
// HELPER - BUTTON NAME
// ============================================================================

static const char* honda_v5_get_button_name(uint8_t btn) {
    switch(btn) {
    case 0x21:
        return "LOCK";
    case 0x22:
        return "UNLOCK";
    default:
        return "?";
    }
}

// ============================================================================
// HELPER - D-PAD BUTTON CODE
// ============================================================================

static uint8_t honda_v5_get_btn_code(void) {
    uint8_t original_btn = subghz_custom_btn_get_original();
    uint8_t c = subghz_custom_btn_get();

    if(c == SUBGHZ_CUSTOM_BTN_OK) return original_btn;

    switch(c) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x22; // UNLOCK
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x21; // LOCK
    default:
        break;
    }

    return original_btn;
}

// ============================================================================
// CRC-8 (poly=0x2F, init=0x03) — CRC-8/AUTOSAR variant
// ============================================================================

/**
 * Compute CRC-8 over raw frame bytes 0-15 (all fields except the CRC byte itself).
 * Polynomial 0x2F, init=0x03, no XOR-out, no reflection.
 * This also works for RTX frames (byte 3 = 0x0A) — the RTX tail emerges naturally.
 */
static uint8_t honda_v5_compute_crc(const uint8_t* data, size_t len) {
    uint8_t crc = 0x03;
    for(size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for(int j = 0; j < 8; ++j) {
            crc = (crc & 0x80) ? (uint8_t)(((crc << 1) ^ 0x2F) & 0xFF)
                               : (uint8_t)((crc << 1) & 0xFF);
        }
    }
    return crc;
}

// ============================================================================
// CRC-8 reflected (poly=0x4D, init=0x00) — used by alternate 168-bit variant
// ============================================================================

static uint8_t honda_v5_compute_crc_alt(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for(int j = 0; j < 8; ++j) {
            crc = (crc & 0x01) ? (uint8_t)(((crc >> 1) ^ 0x4D) & 0xFF)
                               : (uint8_t)((crc >> 1) & 0xFF);
        }
    }
    return crc;
}

// ============================================================================
// HELPER - PROCESS DECODED FRAME
// ============================================================================

/**
 * Process the bits[] array after Manchester decoding.
 * Tries two variants:
 *   1) Standard V5: 135 data bits, 1-bit offset packing, header 0x6C0F62
 *   2) Alt variant: 168 data bits, 8-bit aligned, header 0x6428
 */
static void honda_v5_process_frame(SubGhzProtocolDecoderHondaV5* instance) {
    // Skip leading 1-bits (preamble)
    uint16_t start = 0;
    while(start < instance->bit_count && instance->bits[start] == 1) {
        start++;
    }

    uint16_t data_len = instance->bit_count - start;

    // --- Try alternate 168-bit variant first (needs more bits) ---
    if(data_len >= HONDA_V5_ALT_DATA_BITS) {
        memset(instance->raw_data, 0, sizeof(instance->raw_data));

        // Pack 168 bits into 21 bytes, 8-bit aligned (no offset)
        for(uint16_t i = 0; i < HONDA_V5_ALT_DATA_BITS && (start + i) < instance->bit_count;
            i++) {
            uint8_t byte_idx = i / 8;
            uint8_t bit_idx = 7 - (i % 8);
            if(byte_idx < HONDA_V5_ALT_DATA_BYTES && instance->bits[start + i]) {
                instance->raw_data[byte_idx] |= (1 << bit_idx);
            }
        }

        uint16_t alt_header =
            ((uint16_t)instance->raw_data[0] << 8) | instance->raw_data[1];

        if(alt_header == HONDA_V5_ALT_HEADER) {
            // CRC-8 reflected over bytes 0-19, CRC at byte 20
            uint8_t computed_crc = honda_v5_compute_crc_alt(instance->raw_data, 20);
            uint8_t frame_crc = instance->raw_data[20];

            instance->is_alt = true;
            instance->header = alt_header;
            instance->tail = frame_crc;
            instance->crc_ok = (computed_crc == frame_crc);

            // Alt variant field extraction:
            // Bytes 2-12: encrypted rolling (11 bytes)
            // Bytes 13-17: semi-fixed (serial/button/counter region)
            // Byte 14: likely counter LSB (observed +1 between presses)
            // Byte 19: 0x7D constant

            // Pack bytes 2-9 into generic.data for key display
            instance->generic.data = 0;
            for(int i = 2; i < 10; i++) {
                instance->generic.data = (instance->generic.data << 8) | instance->raw_data[i];
            }
            instance->generic.data_count_bit = HONDA_V5_ALT_DATA_BITS;

            // Extract semi-fixed region as serial (bytes 13-16, 32-bit)
            instance->serial =
                ((uint32_t)instance->raw_data[13] << 24) |
                ((uint32_t)instance->raw_data[14] << 16) |
                ((uint32_t)instance->raw_data[15] << 8) |
                instance->raw_data[16];
            instance->generic.serial = instance->serial;

            // Counter candidate: byte 14 (observed +1 between keypresses)
            instance->counter = instance->raw_data[14];
            instance->generic.cnt = instance->counter;

            // Rolling: bytes 2-9 (encrypted)
            instance->rolling =
                ((uint32_t)instance->raw_data[2] << 24) |
                ((uint32_t)instance->raw_data[3] << 16) |
                ((uint32_t)instance->raw_data[4] << 8) |
                instance->raw_data[5];

            instance->button = instance->raw_data[17];
            instance->generic.btn = instance->button;
            instance->retransmit = 0;
            instance->padding = 0;

            instance->generic.crc = computed_crc;
            instance->generic.crc_check = instance->crc_ok;

            // [FP-FIX 2026-09-14] Alt 168-bit path was gated only by the 16-bit
            // 0x6428 header (~2^-16) and emitted regardless of CRC. Require the
            // reflected CRC-8 to pass (raises accept to ~2^-24). A 0x6428-header
            // frame can never also match the standard 0x6C0F62 header, so dropping
            // (not falling through) loses no true standard frame.
            if(instance->crc_ok && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
            return;
        }
    }

    // --- Standard V5: 135-bit, 1-bit offset packing ---

    // The preamble absorbs leading data bits (value=1) at the boundary.
    // The first 3 bytes are header 0x6C0F62 = 0b 0110 1100 0000 1111 0110 0010
    // so the first data bit is 0, preceded by up to 2 one-bits (from header byte 0x6C).
    // Prepend the missing 1-bits from preamble slots if available.
    uint16_t missing = 0;
    if(data_len < HONDA_V5_DATA_BITS) {
        missing = HONDA_V5_DATA_BITS - data_len;
    }
    if(missing > 0 && missing <= 2 && start >= missing) {
        for(uint16_t i = 0; i < missing; i++) {
            start--;
            instance->bits[start] = 1;
        }
        data_len = HONDA_V5_DATA_BITS;
    }

    if(data_len < HONDA_V5_DATA_BITS) return;

    // Convert bits to bytes (135 bits -> 17 bytes, MSB-padded)
    // 135 bits = 16*8 + 7, so first byte uses only 7 bits
    memset(instance->raw_data, 0, sizeof(instance->raw_data));

    // Pack 135 bits into 17 bytes: bit 0 is MSB of the 135-bit value
    // Byte 0 gets bits 0-6 (7 bits, padded with 0 MSB -> 8 bits)
    // Byte 1 gets bits 7-14, etc.
    for(uint16_t i = 0; i < HONDA_V5_DATA_BITS && (start + i) < instance->bit_count; i++) {
        uint16_t pos = i + 1; // position in 136-bit (17-byte) space
        uint8_t byte_idx = pos / 8;
        uint8_t bit_idx = 7 - (pos % 8);
        if(byte_idx < HONDA_V5_DATA_BYTES) {
            instance->raw_data[byte_idx] |= (instance->bits[start + i] << bit_idx);
        }
    }

    // Extract header (bytes 0-2)
    uint32_t header = ((uint32_t)instance->raw_data[0] << 16) |
                      ((uint32_t)instance->raw_data[1] << 8) | instance->raw_data[2];

    // Validate header
    if(header != HONDA_V5_HEADER_VALUE) return;

    // Extract all fields
    instance->is_alt = false;
    instance->header = header;
    instance->retransmit = instance->raw_data[3];
    instance->serial = ((uint32_t)instance->raw_data[4] << 24) |
                       ((uint32_t)instance->raw_data[5] << 16) |
                       ((uint32_t)instance->raw_data[6] << 8) | instance->raw_data[7];
    instance->button = instance->raw_data[8];
    instance->padding = instance->raw_data[9];
    instance->counter = ((uint16_t)instance->raw_data[10] << 8) | instance->raw_data[11];
    instance->rolling = ((uint32_t)instance->raw_data[12] << 24) |
                        ((uint32_t)instance->raw_data[13] << 16) |
                        ((uint32_t)instance->raw_data[14] << 8) | instance->raw_data[15];
    instance->tail = instance->raw_data[16];

    // Pack into generic: serial(32) + rolling(32) = 64 bits
    instance->generic.data = ((uint64_t)instance->serial << 32) | instance->rolling;
    instance->generic.data_count_bit = HONDA_V5_DATA_BITS;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->button;
    instance->generic.cnt = instance->counter;

    // CRC verification: CRC-8(poly=0x2F, init=0x03) over raw bytes 0-15
    uint8_t computed_crc = honda_v5_compute_crc(instance->raw_data, 16);
    instance->crc_ok = (computed_crc == instance->tail);
    instance->generic.crc = computed_crc;
    instance->generic.crc_check = instance->crc_ok;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// DECODER - TIMING DECODE
// ============================================================================

static void
    honda_v5_decode_timing(SubGhzProtocolDecoderHondaV5* instance, bool level, uint32_t duration) {
    const SubGhzBlockConst* bc = &subghz_protocol_honda_v5_const;

    uint8_t timing_type;
    bool valid = false;

    if(DURATION_DIFF(duration, bc->te_short) <= bc->te_delta) {
        timing_type = level ? 0 : 2;
        valid = true;
    } else if(DURATION_DIFF(duration, bc->te_long) <= bc->te_delta) {
        timing_type = level ? 4 : 6;
        valid = true;
    }

    if(valid) {
        uint8_t bit_value = 0;
        if(honda_v5_manchester_decode(&instance->manchester_state, timing_type, &bit_value)) {
            if(instance->bit_count < HONDA_V5_MAX_BITS) {
                instance->bits[instance->bit_count++] = bit_value;
            }

            if(instance->bit_count >= HONDA_V5_MAX_BITS) {
                honda_v5_process_frame(instance);
                instance->decoder.parser_step = HondaV5DecoderStepReset;
                instance->bit_count = 0;
                instance->manchester_state = 1;
            }
        }
    } else {
        // Invalid timing = gap/end of frame
        if(instance->bit_count >= subghz_protocol_honda_v5_const.min_count_bit_for_found) {
            honda_v5_process_frame(instance);
        }
        instance->decoder.parser_step = HondaV5DecoderStepReset;
        instance->bit_count = 0;
        instance->manchester_state = 1;
    }
}

// ============================================================================
// DECODER - ALLOC / FREE / RESET
// ============================================================================

void* subghz_protocol_decoder_honda_v5_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaV5* instance = malloc(sizeof(SubGhzProtocolDecoderHondaV5));
    instance->base.protocol = &subghz_protocol_honda_v5;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_state = 1;
    instance->bit_count = 0;
    memset(instance->bits, 0, sizeof(instance->bits));
    memset(instance->raw_data, 0, sizeof(instance->raw_data));

    return instance;
}

void subghz_protocol_decoder_honda_v5_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;
    free(instance);
}

void subghz_protocol_decoder_honda_v5_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;
    instance->decoder.parser_step = HondaV5DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->manchester_state = 1;
    instance->bit_count = 0;
}

// ============================================================================
// DECODER - FEED (2-state machine: Reset + Data)
// ============================================================================

void subghz_protocol_decoder_honda_v5_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;

    switch(instance->decoder.parser_step) {
    case HondaV5DecoderStepReset: {
        // Detect start: te_short match starts data collection
        if(DURATION_DIFF(duration, subghz_protocol_honda_v5_const.te_short) <=
           subghz_protocol_honda_v5_const.te_delta) {
            instance->decoder.parser_step = HondaV5DecoderStepData;
            instance->manchester_state = 1;
            instance->bit_count = 0;
            honda_v5_decode_timing(instance, level, duration);
        }
    } break;

    case HondaV5DecoderStepData: {
        // Gap detection: anything much longer than te_long is a frame boundary
        if(duration > (uint32_t)(subghz_protocol_honda_v5_const.te_long +
                                 subghz_protocol_honda_v5_const.te_delta * 3)) {
            if(instance->bit_count >= subghz_protocol_honda_v5_const.min_count_bit_for_found) {
                honda_v5_process_frame(instance);
            }
            instance->decoder.parser_step = HondaV5DecoderStepReset;
            instance->bit_count = 0;
            instance->manchester_state = 1;
            return;
        }

        honda_v5_decode_timing(instance, level, duration);
    } break;

    default:
        instance->decoder.parser_step = HondaV5DecoderStepReset;
        break;
    }
}

// ============================================================================
// DECODER - GET HASH DATA (grouped by counter to prevent burst duplicates)
// ============================================================================

uint8_t subghz_protocol_decoder_honda_v5_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;
    return (uint8_t)(instance->counter & 0xFF);
}

// ============================================================================
// DECODER - SERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_honda_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    // Write extra fields
    uint32_t header_temp = instance->header;
    if(!flipper_format_write_uint32(flipper_format, "Header", &header_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    uint32_t retransmit_temp = instance->retransmit;
    if(!flipper_format_write_uint32(flipper_format, "Retransmit", &retransmit_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    uint32_t rolling_temp = instance->rolling;
    if(!flipper_format_write_uint32(flipper_format, "Rolling", &rolling_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    uint32_t tail_temp = instance->tail;
    if(!flipper_format_write_uint32(flipper_format, "Tail", &tail_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    uint32_t button_temp = instance->button;
    if(!flipper_format_write_uint32(flipper_format, "Button", &button_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    uint32_t counter_temp = instance->counter;
    if(!flipper_format_write_uint32(flipper_format, "Counter", &counter_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    // Disp field for history list
    FuriString* disp_str = furi_string_alloc();
    furi_string_printf(
        disp_str,
        "HondaV5 - %s %s",
        honda_v5_get_button_name(instance->button),
        (instance->retransmit == HONDA_V5_RETRANSMIT_RTX) ? "RTX" : "TX");
    if(!flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp_str))) {
        furi_string_free(disp_str);
    }
    furi_string_free(disp_str);

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DECODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
        if(ret != SubGhzProtocolStatusOk) break;

        // Extract serial and rolling from Key (generic.data)
        instance->serial = (uint32_t)(instance->generic.data >> 32);
        instance->rolling = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

        // Read extra fields
        uint32_t header_temp = HONDA_V5_HEADER_VALUE;
        flipper_format_read_uint32(flipper_format, "Header", &header_temp, 1);
        instance->header = header_temp;

        uint32_t retransmit_temp = HONDA_V5_RETRANSMIT_TX;
        flipper_format_read_uint32(flipper_format, "Retransmit", &retransmit_temp, 1);
        instance->retransmit = (uint8_t)retransmit_temp;

        uint32_t rolling_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Rolling", &rolling_temp, 1)) {
            instance->rolling = rolling_temp;
        }

        uint32_t tail_temp = 0;
        flipper_format_read_uint32(flipper_format, "Tail", &tail_temp, 1);
        instance->tail = (uint8_t)tail_temp;

        uint32_t button_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Button", &button_temp, 1)) {
            instance->button = (uint8_t)button_temp;
            instance->generic.btn = instance->button;
        } else {
            instance->button = instance->generic.btn;
        }

        uint32_t counter_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Counter", &counter_temp, 1)) {
            instance->counter = (uint16_t)counter_temp;
            instance->generic.cnt = instance->counter;
        } else {
            instance->counter = (uint16_t)instance->generic.cnt;
        }

        instance->generic.serial = instance->serial;

        subghz_custom_btn_set_original(instance->button);
        subghz_custom_btn_set_max(2);

    } while(false);

    return ret;
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_honda_v5_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV5* instance = context;

    // Push protocol data to global for UI
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 8;
    subghz_block_generic_global.current_btn = instance->button;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.cnt_length_bit = 16;
    subghz_block_generic_global.current_cnt = instance->counter;

    if(instance->is_alt) {
        // Alternate 168-bit variant
        furi_string_cat_printf(
            output,
            "HondaV5 168bit\r\n"
            "Fix:%08lX Cnt:%02X\r\n"
            "Btn:%02X CRC:%02X(%s)\r\n",
            (unsigned long)instance->serial,
            instance->counter,
            instance->button,
            instance->tail,
            instance->crc_ok ? "OK" : "FAIL");

        // Show encrypted payload bytes 2-12 as hex
        furi_string_cat_printf(output, "Enc:");
        for(int i = 2; i < 13; i++) {
            furi_string_cat_printf(output, "%02X", instance->raw_data[i]);
        }
        furi_string_cat_printf(output, "\r\n");
    } else {
        uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
        uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

        const char* rt_str =
            (instance->retransmit == HONDA_V5_RETRANSMIT_RTX) ? "RTX" : "TX";

        furi_string_cat_printf(
            output,
            "HondaV5 135bit\r\n"
            "Key:%08lX%08lX\r\n"
            "Sn:%08lX Btn:%s\r\n"
            "Cnt:%lu RT:%s\r\n"
            "Roll:%08lX CRC:%02X(%s)\r\n",
            (unsigned long)key_hi,
            (unsigned long)key_lo,
            (unsigned long)instance->serial,
            honda_v5_get_button_name(instance->button),
            (unsigned long)instance->counter,
            rt_str,
            (unsigned long)instance->rolling,
            instance->tail,
            instance->crc_ok ? "OK" : "FAIL");
    }
}

// ============================================================================
// MANCHESTER ENCODER (same state-machine as fiat_encode_bit)
// ============================================================================

static size_t
    honda_v5_encode_bit(LevelDuration* buf, uint8_t* state, uint8_t bit, uint16_t te_s, uint16_t te_l) {
    size_t count = 0;

    switch(*state) {
    case 1:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, (uint32_t)te_l);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            *state = 1;
        }
        break;
    case 2:
        if(bit == 0) {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(true, (uint32_t)te_l);
            *state = 1;
        }
        break;
    case 0:
        if(bit == 1) {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            *state = 1;
        } else {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            buf[count++] = level_duration_make(false, (uint32_t)te_l);
            *state = 2;
        }
        break;
    case 3:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            buf[count++] = level_duration_make(true, (uint32_t)te_l);
            *state = 1;
        }
        break;
    default:
        *state = 1;
        break;
    }

    return count;
}

// ============================================================================
// HELPER - BUILD RAW BYTES FROM FIELDS
// ============================================================================

static void honda_v5_build_raw_bytes(
    uint8_t* raw,
    uint32_t header,
    uint8_t retransmit,
    uint32_t serial,
    uint8_t button,
    uint16_t counter,
    uint32_t rolling,
    uint8_t tail) {
    raw[0] = (header >> 16) & 0xFF;
    raw[1] = (header >> 8) & 0xFF;
    raw[2] = header & 0xFF;
    raw[3] = retransmit;
    raw[4] = (serial >> 24) & 0xFF;
    raw[5] = (serial >> 16) & 0xFF;
    raw[6] = (serial >> 8) & 0xFF;
    raw[7] = serial & 0xFF;
    raw[8] = button;
    raw[9] = 0x00; // padding
    raw[10] = (counter >> 8) & 0xFF;
    raw[11] = counter & 0xFF;
    raw[12] = (rolling >> 24) & 0xFF;
    raw[13] = (rolling >> 16) & 0xFF;
    raw[14] = (rolling >> 8) & 0xFF;
    raw[15] = rolling & 0xFF;
    raw[16] = tail;
}

// ============================================================================
// HELPER - ENCODE ONE FRAME (preamble + Manchester data for 135 bits)
// ============================================================================

/**
 * Encode a single Honda frame into the upload buffer.
 * Returns the number of LevelDuration entries written.
 *
 * The 135-bit data is packed into 17 bytes (MSB-padded: bit 0 of byte 0 is
 * unused since 17*8=136 > 135). We transmit bits from position 1 of the
 * 136-bit space (skipping the padding 0).
 */
static size_t honda_v5_encode_frame(LevelDuration* buf, uint8_t* raw, uint16_t te_s, uint16_t te_l) {
    size_t idx = 0;
    uint8_t enc_state = 0;

    // Preamble: ~360 short alternating pulses (all 1-bits in Manchester)
    // Must match Manchester encoder's 1-bit convention: LOW-HIGH (state=1)
    for(int p = 0; p < HONDA_V5_PREAMBLE_SHORTS / 2; p++) {
        buf[idx++] = level_duration_make(false, (uint32_t)te_s);
        buf[idx++] = level_duration_make(true, (uint32_t)te_s);
    }

    // Preamble ends at HIGH, encoder state=1 (last half was HIGH).
    // This ensures seamless transition to data bits without merged timings.
    enc_state = 1;

    // Encode 135 data bits from 17 raw bytes
    // 136-bit space: bit 0 is padding (0, not transmitted)
    // bits 1-135 are the data
    for(uint16_t i = 0; i < HONDA_V5_DATA_BITS; i++) {
        uint16_t pos = i + 1; // position in 136-bit (17-byte) space
        uint8_t byte_idx = pos / 8;
        uint8_t bit_idx = 7 - (pos % 8);
        uint8_t bit = (raw[byte_idx] >> bit_idx) & 1;
        idx += honda_v5_encode_bit(&buf[idx], &enc_state, bit, te_s, te_l);
    }

    // Guard: if data ends LOW (enc_state==2), add a short HIGH so the
    // following gap (LOW) doesn't merge with the last data timing and
    // swallow the final Manchester bit.
    if(enc_state == 2) {
        buf[idx++] = level_duration_make(true, (uint32_t)te_s);
    }

    return idx;
}

// ============================================================================
// ENCODER - BUILD UPLOAD (TX + RTX frames)
// ============================================================================

static void honda_v5_build_upload(SubGhzProtocolEncoderHondaV5* instance) {
    furi_assert(instance);
    LevelDuration* buf = instance->encoder.upload;
    size_t idx = 0;

    uint16_t te_s = subghz_protocol_honda_v5_const.te_short;
    uint16_t te_l = subghz_protocol_honda_v5_const.te_long;

    uint8_t raw_tx[HONDA_V5_DATA_BYTES];
    uint8_t raw_rtx[HONDA_V5_DATA_BYTES];

    // Build TX frame bytes (retransmit = 0x08)
    honda_v5_build_raw_bytes(
        raw_tx,
        instance->header,
        HONDA_V5_RETRANSMIT_TX,
        instance->serial,
        instance->button,
        instance->counter,
        instance->rolling,
        instance->tail);

    // Build RTX frame bytes (retransmit = 0x0A, tail XOR 0xC4)
    honda_v5_build_raw_bytes(
        raw_rtx,
        instance->header,
        HONDA_V5_RETRANSMIT_RTX,
        instance->serial,
        instance->button,
        instance->counter,
        instance->rolling,
        instance->tail ^ HONDA_V5_TAIL_XOR);

    // Frame 1: TX
    idx += honda_v5_encode_frame(&buf[idx], raw_tx, te_s, te_l);

    // Inter-frame gap
    buf[idx++] = level_duration_make(false, 10000);

    // Frame 2: RTX
    idx += honda_v5_encode_frame(&buf[idx], raw_rtx, te_s, te_l);

    // Trailing gap
    buf[idx++] = level_duration_make(false, 10000);

    instance->encoder.size_upload = idx;
}

// ============================================================================
// ENCODER - ALLOC / FREE / STOP / YIELD
// ============================================================================

void* subghz_protocol_encoder_honda_v5_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHondaV5* instance = malloc(sizeof(SubGhzProtocolEncoderHondaV5));

    instance->base.protocol = &subghz_protocol_honda_v5;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 1; // TX+RTX in one upload, single pass
    // 2 x (preamble 720 + data ~270 + gap 1) + margin
    instance->encoder.size_upload = 2048;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;

    return instance;
}

void subghz_protocol_encoder_honda_v5_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV5* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_honda_v5_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV5* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v5_yield(void* context) {
    SubGhzProtocolEncoderHondaV5* instance = context;

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

// ============================================================================
// ENCODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v5_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV5* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize generic failed");
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind failed");
            break;
        }

        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Missing Key");
            break;
        }

        instance->generic.data = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->generic.data = instance->generic.data << 8 | key_data[i];
        }

        // Extract serial and rolling from Key
        instance->serial = (uint32_t)(instance->generic.data >> 32);
        instance->rolling = (uint32_t)(instance->generic.data & 0xFFFFFFFF);
        instance->generic.serial = instance->serial;

        // Read extra fields
        uint32_t header_temp = HONDA_V5_HEADER_VALUE;
        flipper_format_read_uint32(flipper_format, "Header", &header_temp, 1);
        instance->header = header_temp;

        uint32_t retransmit_temp = HONDA_V5_RETRANSMIT_TX;
        flipper_format_read_uint32(flipper_format, "Retransmit", &retransmit_temp, 1);
        instance->retransmit = (uint8_t)retransmit_temp;

        uint32_t rolling_temp = instance->rolling;
        flipper_format_read_uint32(flipper_format, "Rolling", &rolling_temp, 1);
        instance->rolling = rolling_temp;

        uint32_t tail_temp = 0;
        flipper_format_read_uint32(flipper_format, "Tail", &tail_temp, 1);
        instance->tail = (uint8_t)tail_temp;

        uint32_t button_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Button", &button_temp, 1)) {
            instance->button = (uint8_t)button_temp;
        } else {
            instance->button = instance->generic.btn;
        }

        uint32_t counter_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Counter", &counter_temp, 1)) {
            instance->counter = (uint16_t)counter_temp;
        } else {
            instance->counter = (uint16_t)instance->generic.cnt;
        }

        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->counter;

        // Button: global override first, D-pad fallback
        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->button);
        }
        subghz_custom_btn_set_max(2);

        {
            uint8_t btn_override = instance->button;
            if(!subghz_block_generic_global_button_override_get(&btn_override)) {
                btn_override = honda_v5_get_btn_code();
            }
            instance->generic.btn = btn_override;
            instance->button = btn_override;
        }

        // Counter: rolling_counter_mult API (16-bit)
        {
            uint32_t cnt = instance->counter;
            if(furi_hal_subghz_get_rolling_counter_mult() != -0x7FFFFFFF) {
                if(!subghz_block_generic_global_counter_override_get(&cnt)) {
                    if((cnt + furi_hal_subghz_get_rolling_counter_mult()) > 0xFFFF) {
                        cnt = 0;
                    } else {
                        cnt += furi_hal_subghz_get_rolling_counter_mult();
                    }
                }
            } else {
                cnt = (cnt + 1) & 0xFFFF;
            }
            instance->generic.cnt = cnt & 0xFFFF;
            instance->counter = cnt & 0xFFFF;
        }

        // Compute CRC for the new frame (rolling code stays unchanged — unknown algorithm)
        {
            uint8_t raw_crc[17];
            honda_v5_build_raw_bytes(
                raw_crc,
                instance->header,
                HONDA_V5_RETRANSMIT_TX,
                instance->serial,
                instance->button,
                instance->counter,
                instance->rolling,
                0); // tail placeholder, not included in CRC input
            instance->tail = honda_v5_compute_crc(raw_crc, 16);
        }

        instance->generic.data = ((uint64_t)instance->serial << 32) | instance->rolling;
        instance->generic.data_count_bit = HONDA_V5_DATA_BITS;

        // Optional repeat
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Build upload (TX + RTX frames)
        honda_v5_build_upload(instance);

        // Write back updated Key to file
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind for update failed");
            break;
        }
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Update Key failed");
            break;
        }

        // Write updated Counter back
        {
            uint32_t cnt_write = instance->counter;
            flipper_format_update_uint32(flipper_format, "Counter", &cnt_write, 1);
        }

        // Write updated Button back
        {
            uint32_t btn_write = instance->button;
            flipper_format_update_uint32(flipper_format, "Button", &btn_write, 1);
        }

        // Write updated Rolling and Tail back
        {
            uint32_t roll_write = instance->rolling;
            flipper_format_update_uint32(flipper_format, "Rolling", &roll_write, 1);
            uint32_t tail_write = instance->tail;
            flipper_format_update_uint32(flipper_format, "Tail", &tail_write, 1);
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;

    } while(false);

    return res;
}
