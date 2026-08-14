#include "land_rover_rke.h"
#include "protocols_common.h"

#include <string.h>

#define TAG "LandRoverRke"

// ---- Timing constants (from KAT src/protocols/land_rover_rke.rs) ----
// Fixed-width PWM, 1000us bit: bit=1 is 700us HIGH + 300us LOW, bit=0 is
// 300us HIGH + 700us LOW. Tolerance ~20%.
static const SubGhzBlockConst subghz_protocol_land_rover_rke_const = {
    .te_short = 300,
    .te_long = 700,
    .te_delta = 140, // ~20% of te_long / ~47% of te_short (matches land_rover_v0's asymmetric-delta convention)
    .min_count_bit_for_found = 66,
};

// Preamble: >=16 (up to 20) pairs of 400us HIGH + 600us LOW.
#define LAND_ROVER_RKE_PREAMBLE_HIGH_US   400U
#define LAND_ROVER_RKE_PREAMBLE_LOW_US    600U
#define LAND_ROVER_RKE_PREAMBLE_DELTA_US  100U
#define LAND_ROVER_RKE_MIN_PREAMBLE_PAIRS 16U

// Sync: 400us HIGH (same nominal as a preamble pulse) + 9600us LOW. The long
// 9600us gap is the key discriminator that gates false matches.
#define LAND_ROVER_RKE_SYNC_LOW_US       9600U
#define LAND_ROVER_RKE_SYNC_LOW_DELTA_US 1920U // ~20% of 9600

typedef struct SubGhzProtocolDecoderLandRoverRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;

    // 66-bit frame accumulator: extra_bits holds V[65:64], data holds V[63:0].
    uint64_t data;
    uint8_t extra_bits;
    uint8_t bit_count;
    bool pending_bit;

    uint32_t hop; // 32-bit KeeLoq hop — ENCRYPTED, surfaced raw, never decrypted
    uint32_t serial; // 24-bit
    uint8_t button; // 4-bit
    uint8_t flags; // 4-bit
    uint8_t status; // 2-bit
} SubGhzProtocolDecoderLandRoverRke;

typedef enum {
    LandRoverRkeDecoderStepReset = 0,
    LandRoverRkeDecoderStepPreambleLow,
    LandRoverRkeDecoderStepPreambleHigh,
    LandRoverRkeDecoderStepDataHigh,
    LandRoverRkeDecoderStepDataLow,
} LandRoverRkeDecoderStep;

static const char* land_rover_rke_button_name(uint8_t button) {
    switch(button) {
    case 0x1:
        return "Lock";
    case 0x2:
        return "Unlock";
    case 0x4:
        return "Boot";
    case 0x8:
        return "Panic";
    default:
        return "Unknown";
    }
}

static bool land_rover_rke_matches(uint32_t duration, uint32_t nominal, uint32_t delta) {
    return DURATION_DIFF(duration, nominal) < delta;
}

static bool land_rover_rke_is_preamble_high(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_PREAMBLE_HIGH_US, LAND_ROVER_RKE_PREAMBLE_DELTA_US);
}

static bool land_rover_rke_is_preamble_low(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_PREAMBLE_LOW_US, LAND_ROVER_RKE_PREAMBLE_DELTA_US);
}

static bool land_rover_rke_is_sync_low(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_SYNC_LOW_US, LAND_ROVER_RKE_SYNC_LOW_DELTA_US);
}

static bool land_rover_rke_is_bit_long(uint32_t duration) {
    return pp_is_long(duration, &subghz_protocol_land_rover_rke_const);
}

static bool land_rover_rke_is_bit_short(uint32_t duration) {
    return pp_is_short(duration, &subghz_protocol_land_rover_rke_const);
}

static void
    land_rover_rke_shift_in_bit(SubGhzProtocolDecoderLandRoverRke* instance, bool bit) {
    const uint8_t carry = (uint8_t)((instance->data >> 63) & 1U);
    instance->data = (instance->data << 1) | (bit ? 1ULL : 0ULL);
    instance->extra_bits = (uint8_t)(((instance->extra_bits << 1) | carry) & 0x3U);
    instance->bit_count++;
}

// Extracts hop/serial/button/flags/status from the 66-bit frame
// (extra_bits:data == V[65:0], MSB-first per the KAT layout) into the
// instance fields. Returns false (no fields written beyond zero) if the
// serial is zero — used both to gate "found" and to recompute display
// fields after deserialize.
static bool land_rover_rke_extract_fields(uint8_t extra_bits, uint64_t data, uint32_t* hop_out,
    uint32_t* serial_out, uint8_t* button_out, uint8_t* flags_out, uint8_t* status_out) {
    const uint32_t hop = ((uint32_t)(extra_bits & 0x3U) << 30) |
                          (uint32_t)((data >> 34) & 0x3FFFFFFFULL);
    const uint32_t serial = (uint32_t)((data >> 10) & 0xFFFFFFULL);
    const uint8_t button = (uint8_t)((data >> 6) & 0xFULL);
    const uint8_t flags = (uint8_t)((data >> 2) & 0xFULL);
    const uint8_t status = (uint8_t)(data & 0x3ULL);

    if(hop_out) *hop_out = hop;
    if(serial_out) *serial_out = serial;
    if(button_out) *button_out = button;
    if(flags_out) *flags_out = flags;
    if(status_out) *status_out = status;

    return serial != 0U;
}

static bool land_rover_rke_finish_frame(SubGhzProtocolDecoderLandRoverRke* instance) {
    if(!land_rover_rke_extract_fields(
           instance->extra_bits,
           instance->data,
           &instance->hop,
           &instance->serial,
           &instance->button,
           &instance->flags,
           &instance->status)) {
        return false; // serial == 0 -> reject (spec: "Serial must be non-zero")
    }

    instance->generic.data = instance->data;
    instance->generic.data_count_bit = subghz_protocol_land_rover_rke_const.min_count_bit_for_found;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->button;
    instance->generic.cnt = instance->hop; // raw ENCRYPTED hop, not decoded
    instance->generic.crc_check = false; // decode-only: no crypto/CRC validation possible

    instance->decoder.decode_data = instance->data;
    instance->decoder.decode_count_bit = subghz_protocol_land_rover_rke_const.min_count_bit_for_found;

    return true;
}

const SubGhzProtocolDecoder subghz_protocol_land_rover_rke_decoder = {
    .alloc = subghz_protocol_decoder_land_rover_rke_alloc,
    .free = subghz_protocol_decoder_land_rover_rke_free,
    .feed = subghz_protocol_decoder_land_rover_rke_feed,
    .reset = subghz_protocol_decoder_land_rover_rke_reset,
    .get_hash_data = subghz_protocol_decoder_land_rover_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_land_rover_rke_serialize,
    .deserialize = subghz_protocol_decoder_land_rover_rke_deserialize,
    .get_string = subghz_protocol_decoder_land_rover_rke_get_string,
};

// Decode-only: no key material is resident in this firmware for the KeeLoq
// hop, so there is nothing to encode. Mirrors fiat_v0.c's unconditional
// NULL-upload stub, except this protocol has no ENABLE_EMULATE_FEATURE path
// at all (unlike land_rover_v0, which *can* forge its unencrypted key).
const SubGhzProtocolEncoder subghz_protocol_land_rover_rke_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol land_rover_rke_protocol = {
    .name = LAND_ROVER_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable,
    .decoder = &subghz_protocol_land_rover_rke_decoder,
    .encoder = &subghz_protocol_land_rover_rke_encoder,
};

void* subghz_protocol_decoder_land_rover_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLandRoverRke* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderLandRoverRke));
    furi_check(instance);

    instance->base.protocol = &land_rover_rke_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_land_rover_rke_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;
    free(instance);
}

void subghz_protocol_decoder_land_rover_rke_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->preamble_count = 0;
    instance->data = 0;
    instance->extra_bits = 0;
    instance->bit_count = 0;
    instance->pending_bit = false;
}

void subghz_protocol_decoder_land_rover_rke_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    switch(instance->decoder.parser_step) {
    case LandRoverRkeDecoderStepReset:
        if(level && land_rover_rke_is_preamble_high(duration)) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleLow;
        }
        break;

    case LandRoverRkeDecoderStepPreambleLow:
        if(!level && land_rover_rke_is_preamble_low(duration)) {
            instance->preamble_count++;
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleHigh;
        } else if(
            !level && land_rover_rke_is_sync_low(duration) &&
            instance->preamble_count >= LAND_ROVER_RKE_MIN_PREAMBLE_PAIRS) {
            // Sync detected: 400us HIGH (already consumed as a preamble-shaped
            // pulse) followed by the discriminating 9600us LOW. Start data.
            instance->data = 0;
            instance->extra_bits = 0;
            instance->bit_count = 0;
            instance->pending_bit = false;
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataHigh;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepPreambleHigh:
        if(level && land_rover_rke_is_preamble_high(duration)) {
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleLow;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepDataHigh:
        if(level && land_rover_rke_is_bit_long(duration)) {
            instance->pending_bit = true; // 700us HIGH -> bit 1
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataLow;
        } else if(level && land_rover_rke_is_bit_short(duration)) {
            instance->pending_bit = false; // 300us HIGH -> bit 0
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataLow;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepDataLow: {
        const bool low_ok = !level &&
                             (instance->pending_bit ? land_rover_rke_is_bit_short(duration) :
                                                       land_rover_rke_is_bit_long(duration));
        if(!low_ok) {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
            break;
        }

        land_rover_rke_shift_in_bit(instance, instance->pending_bit);

        if(instance->bit_count == subghz_protocol_land_rover_rke_const.min_count_bit_for_found) {
            if(land_rover_rke_finish_frame(instance) && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataHigh;
        }
        break;
    }
    default:
        break;
    }

    instance->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_land_rover_rke_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;
    uint8_t hash = subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
    hash ^= instance->extra_bits;
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        pp_flipper_update_or_insert_u32(flipper_format, FF_SERIAL, instance->serial);
        pp_flipper_update_or_insert_u32(flipper_format, FF_BTN, instance->button);
        pp_flipper_update_or_insert_u32(flipper_format, "Hop", instance->hop);
        pp_flipper_update_or_insert_u32(flipper_format, "Flags", instance->flags);
        pp_flipper_update_or_insert_u32(flipper_format, "Status", instance->status);
        pp_flipper_update_or_insert_u32(flipper_format, "ExtraBits", instance->extra_bits);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_land_rover_rke_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        instance->data = instance->generic.data;

        uint32_t extra_bits_read = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "ExtraBits", &extra_bits_read, 1)) {
            instance->extra_bits = (uint8_t)(extra_bits_read & 0x3U);
        } else {
            instance->extra_bits = 0;
        }

        land_rover_rke_extract_fields(
            instance->extra_bits,
            instance->data,
            &instance->hop,
            &instance->serial,
            &instance->button,
            &instance->flags,
            &instance->status);

        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->hop;
        instance->generic.crc_check = false;
    }

    return ret;
}

void subghz_protocol_decoder_land_rover_rke_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%06lX  Btn:%01X - %s\r\n"
        "Hop(raw,ENC):%08lX\r\n"
        "Flags:%01X  Status:%01X\r\n"
        "Crc:NOT CHECKED (decode-only, no key)\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)instance->serial,
        instance->button,
        land_rover_rke_button_name(instance->button),
        (unsigned long)instance->hop,
        instance->flags,
        instance->status);
}
