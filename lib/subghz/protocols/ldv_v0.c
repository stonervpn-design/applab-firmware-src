/**
 * @file ldv_v0.c
 * @brief LDV V0 SubGHz Protocol Decoder (experimental)
 *
 * LDV T80 keyfob — 433.889 MHz, OOK, Manchester encoding.
 * Same timings as Ford V0 (te_short=250us, te_long=500us).
 *
 * Frame structure (128 data bits after sync):
 *   Preamble: >48 Manchester 1-bits (alternating SHORT pairs)
 *   Sync:     First 0-bit after preamble (FE boundary)
 *   Data:     128 bits (16 bytes), all plaintext except hop:
 *     Bytes 0-5:   Serial (48 bits, fixed)
 *     Byte 6:      0x80 | (btn << 3) | 0x07  (btn: 1=Unlock, 2=Lock, 3=Trunk?)
 *     Bytes 7-8:   0xFF 0xFF (fixed separator)
 *     Bytes 9-10:  (counter << 5) | (btn << 3) | 0x07  (11-bit counter, decrements)
 *     Bytes 11-15: Rolling code (40 bits, non-linear PRNG, NOT simple XOR)
 *
 * Button is duplicated in byte 6 and bytes 9-10 (validation).
 * Counter is 11-bit, decrements by 1 per button press.
 */

#include "ldv_v0.h"
#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "LdvV0"

static const SubGhzBlockConst subghz_protocol_ldv_v0_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 120,
    .min_count_bit_for_found = 128,
};

// ============================================================================
// Field extraction helpers
// ============================================================================

static const char* ldv_v0_get_button_name(uint8_t btn) {
    switch(btn) {
    case 1:
        return "Unlock";
    case 2:
        return "Lock";
    case 3:
        return "Trunk";
    default:
        return "??";
    }
}

// ============================================================================
// Decoder state machine
// ============================================================================

struct SubGhzProtocolDecoderLdvV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;
    ManchesterState manchester_saved_state;
    bool last_bit_value;
    uint64_t data_2; // second 64 bits of 128-bit frame
};

struct SubGhzProtocolEncoderLdvV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint64_t data_2; // second 64 bits of 128-bit frame
};

// Preamble Manchester 1-bits emitted before the sync 0-bit. The decoder needs
// >= 48 (see LdvV0StepPreamble); 60 leaves comfortable margin.
#define LDV_V0_PREAMBLE_BITS 60
// Reset-trigger (1) + preamble+sync+128 data bits (<= 2 pulses each) + end gap (1).
#define LDV_V0_UPLOAD_MAX    512

typedef enum {
    LdvV0StepReset = 0,
    LdvV0StepPreamble,
    LdvV0StepData,
} LdvV0DecoderStep;

// ============================================================================
// Protocol registration
// ============================================================================

#ifdef SUBGHZ_DECODER_TEST
static SubGhzBlockGeneric* subghz_protocol_decoder_ldv_v0_get_generic(void* context) {
    SubGhzProtocolDecoderLdvV0* instance = context;
    return &instance->generic;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_ldv_v0_decoder = {
    .alloc = subghz_protocol_decoder_ldv_v0_alloc,
    .free = subghz_protocol_decoder_ldv_v0_free,

    .feed = subghz_protocol_decoder_ldv_v0_feed,
    .reset = subghz_protocol_decoder_ldv_v0_reset,

    .get_hash_data = subghz_protocol_decoder_ldv_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_ldv_v0_serialize,
    .deserialize = subghz_protocol_decoder_ldv_v0_deserialize,
    .get_string = subghz_protocol_decoder_ldv_v0_get_string,
#ifdef SUBGHZ_DECODER_TEST
    .get_generic = subghz_protocol_decoder_ldv_v0_get_generic,
#endif
};

const SubGhzProtocolEncoder subghz_protocol_ldv_v0_encoder = {
    .alloc = subghz_protocol_encoder_ldv_v0_alloc,
    .free = subghz_protocol_encoder_ldv_v0_free,

    .deserialize = subghz_protocol_encoder_ldv_v0_deserialize,
    .stop = subghz_protocol_encoder_ldv_v0_stop,
    .yield = subghz_protocol_encoder_ldv_v0_yield,
};

const SubGhzProtocol subghz_protocol_ldv_v0 = {
    .name = SUBGHZ_PROTOCOL_LDV_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,

    .decoder = &subghz_protocol_ldv_v0_decoder,
    .encoder = &subghz_protocol_ldv_v0_encoder,
};

// ============================================================================
// Encoder: clones the captured 128-bit frame verbatim (re-emits the stored
// Key/Key_2 bits). The decoder recovers bits through manchester_advance(), so
// the encoder drives a mirror copy of that same FSM: for each desired output
// bit it picks the pulse(s) that make manchester_advance() emit exactly that
// bit. This is the guaranteed inverse of the decoder (no rolling crypto is
// re-run — the stored, already-encrypted hop bits are reproduced as-is).
// ============================================================================

void* subghz_protocol_encoder_ldv_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderLdvV0* instance = malloc(sizeof(SubGhzProtocolEncoderLdvV0));
    instance->base.protocol = &subghz_protocol_ldv_v0;
    instance->generic.protocol_name = subghz_protocol_ldv_v0.name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 0;
    instance->encoder.front = 0;
    instance->encoder.upload = malloc(LDV_V0_UPLOAD_MAX * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->data_2 = 0;
    return instance;
}

void subghz_protocol_encoder_ldv_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderLdvV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

// Map a decoder ManchesterEvent back to the physical (level, duration) pulse
// that produces it. Mirrors the classification in subghz_protocol_decoder_ldv_v0_feed:
//   ShortLow  <- (high, te_short)   ShortHigh <- (low, te_short)
//   LongLow   <- (high, te_long)    LongHigh  <- (low, te_long)
static void ldv_v0_emit_event(
    SubGhzProtocolEncoderLdvV0* instance,
    size_t* index,
    ManchesterEvent event) {
    bool level = false;
    uint32_t duration = 0;
    switch(event) {
    case ManchesterEventShortLow:
        level = true;
        duration = subghz_protocol_ldv_v0_const.te_short;
        break;
    case ManchesterEventShortHigh:
        level = false;
        duration = subghz_protocol_ldv_v0_const.te_short;
        break;
    case ManchesterEventLongLow:
        level = true;
        duration = subghz_protocol_ldv_v0_const.te_long;
        break;
    case ManchesterEventLongHigh:
        level = false;
        duration = subghz_protocol_ldv_v0_const.te_long;
        break;
    default:
        return;
    }
    if(*index < LDV_V0_UPLOAD_MAX) {
        instance->encoder.upload[(*index)++] = level_duration_make(level, duration);
    }
}

// Drive the mirror decoder FSM to output a single Manchester bit `bit`.
// Tries a one-pulse event first, otherwise a priming (non-yielding) event
// followed by a yielding one. Returns false only if no drive exists (never
// happens for this FSM, but guards the caller).
static bool ldv_v0_put_bit(
    SubGhzProtocolEncoderLdvV0* instance,
    size_t* index,
    ManchesterState* state,
    bool bit) {
    static const ManchesterEvent events[4] = {
        ManchesterEventShortLow,
        ManchesterEventShortHigh,
        ManchesterEventLongLow,
        ManchesterEventLongHigh};

    for(int i = 0; i < 4; i++) {
        ManchesterState next;
        bool data;
        if(manchester_advance(*state, events[i], &next, &data) && data == bit) {
            ldv_v0_emit_event(instance, index, events[i]);
            *state = next;
            return true;
        }
    }
    for(int i = 0; i < 4; i++) {
        ManchesterState mid;
        bool d0;
        if(manchester_advance(*state, events[i], &mid, &d0)) continue; // yielded a bit, skip
        for(int j = 0; j < 4; j++) {
            ManchesterState next;
            bool data;
            if(manchester_advance(mid, events[j], &next, &data) && data == bit) {
                ldv_v0_emit_event(instance, index, events[i]);
                ldv_v0_emit_event(instance, index, events[j]);
                *state = next;
                return true;
            }
        }
    }
    return false;
}

static void subghz_protocol_encoder_ldv_v0_get_upload(SubGhzProtocolEncoderLdvV0* instance) {
    furi_assert(instance);
    size_t index = 0;

    // First SHORT HIGH pulse is consumed by the decoder's Reset step (it only
    // arms preamble detection and resets the Manchester state), so it is not
    // part of the Manchester bitstream.
    instance->encoder.upload[index++] =
        level_duration_make(true, subghz_protocol_ldv_v0_const.te_short);

    // The decoder resets its Manchester state to Mid1 on the trigger; mirror it.
    ManchesterState state = ManchesterStateMid1;

    for(int i = 0; i < LDV_V0_PREAMBLE_BITS; i++) {
        ldv_v0_put_bit(instance, &index, &state, true); // preamble 1-bits
    }
    ldv_v0_put_bit(instance, &index, &state, false); // sync 0-bit

    for(int b = 63; b >= 0; b--) {
        ldv_v0_put_bit(instance, &index, &state, (instance->generic.data >> b) & 1);
    }
    for(int b = 63; b >= 0; b--) {
        ldv_v0_put_bit(instance, &index, &state, (instance->data_2 >> b) & 1);
    }

    // End gap (> te_long*4) so the decoder finalizes the frame. Emit it at the
    // OPPOSITE level from the last data pulse: on a real radio (and in the merge-
    // aware harness) two consecutive same-level entries physically coalesce into
    // one pulse. If the final data bit completes on a LOW pulse (a trailing 0-bit)
    // and the gap were also LOW, the two would merge into a single over-long LOW
    // that the decoder reads as the end gap alone — swallowing the 128th bit and
    // dropping the frame below min_count_bit_for_found. An opposite-level gap keeps
    // that completing pulse a distinct transition so all 128 bits are delivered.
    if(index < LDV_V0_UPLOAD_MAX) {
        bool last_level = level_duration_get_level(instance->encoder.upload[index - 1]);
        instance->encoder.upload[index++] =
            level_duration_make(!last_level, subghz_protocol_ldv_v0_const.te_long * 8);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_ldv_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderLdvV0* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_ldv_v0_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        // Read Key_2 (second 64 bits) symmetrically with the decoder serialize.
        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data_2[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_2", key_data_2, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->data_2 = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->data_2 = (instance->data_2 << 8) | key_data_2[i];
        }

        // Optional repeat override.
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_ldv_v0_get_upload(instance);
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_ldv_v0_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderLdvV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_ldv_v0_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderLdvV0* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];
    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.repeat--;
        instance->encoder.front = 0;
    }
    return ret;
}

// ============================================================================
// Field extraction: Serial, Button, Counter from 128-bit frame
// ============================================================================

static void subghz_protocol_ldv_v0_extract_fields(SubGhzProtocolDecoderLdvV0* instance) {
    // Serial: bytes 0-3 of Key (upper 32 bits)
    instance->generic.serial = (uint32_t)(instance->generic.data >> 32);

    // Button: (byte6 >> 3) & 0x03 — byte6 is at bits 15-8 of Key
    uint8_t byte6 = (uint8_t)(instance->generic.data >> 8);
    instance->generic.btn = (byte6 >> 3) & 0x03;

    // Counter: bytes 9-10 as 16-bit >> 5 — byte9 is Key2[8], byte10 is Key2[16]
    // Key2 = data_2: byte8=bits63-56, byte9=bits55-48, byte10=bits47-40
    uint8_t byte9 = (uint8_t)(instance->data_2 >> 48);
    uint8_t byte10 = (uint8_t)(instance->data_2 >> 40);
    uint16_t cnt_field = ((uint16_t)byte9 << 8) | byte10;
    instance->generic.cnt = cnt_field >> 5;
}

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_ldv_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLdvV0* instance = malloc(sizeof(SubGhzProtocolDecoderLdvV0));
    instance->base.protocol = &subghz_protocol_ldv_v0;
    instance->generic.protocol_name = subghz_protocol_ldv_v0.name;
    return instance;
}

void subghz_protocol_decoder_ldv_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_ldv_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;
    instance->decoder.parser_step = LdvV0StepReset;
    instance->preamble_count = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->data_2 = 0;
}

// ============================================================================
// Decoder: feed
// ============================================================================

// Frame validation lifted from get_string: genuine LDV frames carry
//   byte6 = 0x80 | (btn << 3) | 0x07,
//   a fixed 0xFF 0xFF separator in bytes 7-8, and
//   the button duplicated in byte6 and bytes 9-10.
// Uses generic.data (Key = bytes 0-7) and data_2 (Key2 = bytes 8-15).
static bool subghz_protocol_ldv_v0_frame_valid(SubGhzProtocolDecoderLdvV0* instance) {
    uint8_t byte6 = (uint8_t)(instance->generic.data >> 8);
    uint8_t byte7 = (uint8_t)(instance->generic.data);
    uint8_t byte8 = (uint8_t)(instance->data_2 >> 56);
    uint8_t byte9 = (uint8_t)(instance->data_2 >> 48);
    uint8_t byte10 = (uint8_t)(instance->data_2 >> 40);

    // byte6 framing: high bit set, low nibble marker 0x07
    if((byte6 & 0x80) == 0) return false;
    if((byte6 & 0x07) != 0x07) return false;

    // fixed 0xFF 0xFF separator in bytes 7-8
    if(byte7 != 0xFF || byte8 != 0xFF) return false;

    // button duplicated in byte6 and bytes 9-10
    uint8_t byte6_btn = (byte6 >> 3) & 0x03;
    uint16_t cnt_field = ((uint16_t)byte9 << 8) | byte10;
    uint8_t cnt_btn = (cnt_field >> 3) & 0x03;
    if(byte6_btn != cnt_btn) return false;

    return true;
}

void subghz_protocol_decoder_ldv_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    // Classify timing
    if(DURATION_DIFF(duration, subghz_protocol_ldv_v0_const.te_short) <
       subghz_protocol_ldv_v0_const.te_delta) {
        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    } else if(
        DURATION_DIFF(duration, subghz_protocol_ldv_v0_const.te_long) <
        subghz_protocol_ldv_v0_const.te_delta) {
        event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }

    switch(instance->decoder.parser_step) {
    case LdvV0StepReset:
        // Wait for first valid SHORT HIGH to start preamble detection
        if(level &&
           DURATION_DIFF(duration, subghz_protocol_ldv_v0_const.te_short) <
               subghz_protocol_ldv_v0_const.te_delta) {
            instance->preamble_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->data_2 = 0;
            instance->decoder.parser_step = LdvV0StepPreamble;
            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);
        }
        break;

    case LdvV0StepPreamble:
        // Large gap → reset
        if(duration > subghz_protocol_ldv_v0_const.te_long * 4) {
            instance->decoder.parser_step = LdvV0StepReset;
            break;
        }

        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = LdvV0StepReset;
            break;
        }

        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state,
                event,
                &instance->manchester_saved_state,
                &data);

            if(data_ok) {
                if(data == instance->last_bit_value && instance->preamble_count > 0) {
                    // Same bit value as before → still in preamble
                    instance->preamble_count++;
                } else if(instance->preamble_count == 0) {
                    // First bit → start counting
                    instance->last_bit_value = data;
                    instance->preamble_count = 1;
                } else {
                    // Bit value changed → this is the sync marker
                    if(instance->preamble_count >= 48) {
                        // Enough preamble, transition to data collection
                        instance->decoder.decode_data = 0;
                        instance->decoder.decode_count_bit = 0;
                        instance->data_2 = 0;
                        instance->decoder.parser_step = LdvV0StepData;
                        FURI_LOG_D(TAG, "Sync after %d preamble bits", instance->preamble_count);
                    } else {
                        // Not enough preamble, restart
                        instance->last_bit_value = data;
                        instance->preamble_count = 1;
                    }
                }
            }
        }
        break;

    case LdvV0StepData:
        // Large gap → frame end, check what we have
        if(duration > subghz_protocol_ldv_v0_const.te_long * 4) {
            if(instance->decoder.decode_count_bit >=
               subghz_protocol_ldv_v0_const.min_count_bit_for_found) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                subghz_protocol_ldv_v0_extract_fields(instance);

                if(subghz_protocol_ldv_v0_frame_valid(instance) && instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = LdvV0StepReset;
            break;
        }

        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = LdvV0StepReset;
            break;
        }

        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state,
                event,
                &instance->manchester_saved_state,
                &data);

            if(data_ok) {
                if(instance->decoder.decode_count_bit < 64) {
                    instance->decoder.decode_data =
                        (instance->decoder.decode_data << 1) | data;
                } else {
                    instance->data_2 = (instance->data_2 << 1) | data;
                }
                instance->decoder.decode_count_bit++;

                if(instance->decoder.decode_count_bit == 128) {
                    // Full frame received
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = 128;

                    // Extract fields
                    subghz_protocol_ldv_v0_extract_fields(instance);

                    if(subghz_protocol_ldv_v0_frame_valid(instance) && instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);

                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 0;
                    instance->data_2 = 0;
                    instance->decoder.parser_step = LdvV0StepReset;
                }
            }
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize
// ============================================================================

uint8_t subghz_protocol_decoder_ldv_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_ldv_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    // Write Key_2 (second 64 bits)
    uint8_t key_data_2[sizeof(uint64_t)] = {0};
    uint64_t data_2_be = __builtin_bswap64(instance->data_2);
    memcpy(key_data_2, &data_2_be, sizeof(key_data_2));
    if(!flipper_format_rewind(flipper_format)) {
        return SubGhzProtocolStatusErrorParserOthers;
    }
    if(!flipper_format_insert_or_update_hex(
           flipper_format, "Key_2", key_data_2, sizeof(uint64_t))) {
        return SubGhzProtocolStatusErrorParserOthers;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ldv_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_ldv_v0_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        // Read Key_2
        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data_2[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key_2", key_data_2, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        instance->data_2 = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->data_2 = (instance->data_2 << 8) | key_data_2[i];
        }

        // Extract fields
        subghz_protocol_ldv_v0_extract_fields(instance);

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

// ============================================================================
// Decoder: get_string
// ============================================================================

void subghz_protocol_decoder_ldv_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderLdvV0* instance = context;

    subghz_protocol_ldv_v0_extract_fields(instance);

    // Serial extension: bytes 4-5 (bits 31-16 of Key)
    uint16_t serial_ext = (uint16_t)(instance->generic.data >> 16) & 0xFFFF;

    // Hop: bytes 11-15 (bits 39-0 of Key2)
    uint64_t hop = instance->data_2 & 0xFFFFFFFFFFULL;

    // Button validation: compare btn in byte6 with btn in bytes9-10
    uint8_t byte6_btn = ((uint8_t)(instance->generic.data >> 8) >> 3) & 0x03;
    uint8_t byte9 = (uint8_t)(instance->data_2 >> 48);
    uint8_t byte10 = (uint8_t)(instance->data_2 >> 40);
    uint16_t cnt_field = ((uint16_t)byte9 << 8) | byte10;
    uint8_t cnt_btn = (cnt_field >> 3) & 0x03;
    bool btn_match = (byte6_btn == cnt_btn);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "Key2:%016llX\r\n"
        "Sn:%08lX%04X Btn:%01X [%s]\r\n"
        "Cnt:%04lX Hop:%010llX [%s]",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->generic.data,
        instance->data_2,
        instance->generic.serial,
        serial_ext,
        instance->generic.btn,
        ldv_v0_get_button_name(instance->generic.btn),
        instance->generic.cnt & 0x7FF,
        hop,
        btn_match ? "OK" : "ERR");
}
