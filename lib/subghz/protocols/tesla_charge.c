#include "tesla_charge.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <lib/toolbox/manchester_decoder.h>

/*
 * Tesla charge-port opener.
 *
 * Fixed (non-rolling) code - identical for every car. OOK/AM.
 *   US:      315.00 MHz
 *   EU/AUS:  433.92 MHz
 *
 * On-air structure (te = 400us, +/-30% tolerated):
 *   - Preamble:  ~12 short high/low cells (0.4ms on, 0.4ms off)
 *   - Sync gap:  one short high pulse then a ~1.2ms (3*te) low gap
 *   - Payload:   50 bits, biphase / Manchester coded at te=400us
 *                (max run length 2*te, transition on every bit boundary)
 *   - The whole burst repeats several times, separated by ~1.2ms gaps.
 *
 * The 50-bit payload decodes (via the Flipper manchester state machine) to a
 * single constant: 0x00036B5564C725E6. This was reconstructed and verified
 * bit-exact against the public Robbbbbbbbb/tesla-chargeport captures
 * (idealized Tesla_Captured.sub and the noisy 315/433 AM270 captures - 20/20
 * repeats decoded identically).
 *
 * Because the code is fixed, the encoder re-synthesises the burst directly from
 * generic.data using the same biphase rule, so a saved capture can be replayed.
 */

#define TAG "SubGhzProtocolTeslaCharge"

// The single fixed 50-bit payload shared by all Tesla charge ports
#define TESLA_CHARGE_FIX_CODE 0x00036B5564C725E6ULL

// Nominal timing (us)
#define TESLA_CHARGE_TE_DEFAULT 400

static const SubGhzBlockConst subghz_protocol_tesla_charge_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 140, // ~35% of te_short
    .min_count_bit_for_found = 50,
};

// Anything longer than this (us) is treated as an inter-burst gap / boundary
#define TESLA_CHARGE_GAP_MIN 1000

struct SubGhzProtocolDecoderTeslaCharge {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
};

struct SubGhzProtocolEncoderTeslaCharge {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t te;
};

const SubGhzProtocolDecoder subghz_protocol_tesla_charge_decoder = {
    .alloc = subghz_protocol_decoder_tesla_charge_alloc,
    .free = subghz_protocol_decoder_tesla_charge_free,

    .feed = subghz_protocol_decoder_tesla_charge_feed,
    .reset = subghz_protocol_decoder_tesla_charge_reset,

    .get_hash_data = subghz_protocol_decoder_tesla_charge_get_hash_data,
    .serialize = subghz_protocol_decoder_tesla_charge_serialize,
    .deserialize = subghz_protocol_decoder_tesla_charge_deserialize,
    .get_string = subghz_protocol_decoder_tesla_charge_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_tesla_charge_encoder = {
    .alloc = subghz_protocol_encoder_tesla_charge_alloc,
    .free = subghz_protocol_encoder_tesla_charge_free,

    .deserialize = subghz_protocol_encoder_tesla_charge_deserialize,
    .stop = subghz_protocol_encoder_tesla_charge_stop,
    .yield = subghz_protocol_encoder_tesla_charge_yield,
};

const SubGhzProtocol subghz_protocol_tesla_charge = {
    .name = TESLA_CHARGE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_tesla_charge_decoder,
    .encoder = &subghz_protocol_tesla_charge_encoder,
};

/* ---------------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------------ */

void* subghz_protocol_encoder_tesla_charge_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderTeslaCharge* instance =
        malloc(sizeof(SubGhzProtocolEncoderTeslaCharge));

    instance->base.protocol = &subghz_protocol_tesla_charge;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 5;
    // preamble(26) + sync + payload(up to 100) + gap; 256 is comfortably safe
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->te = TESLA_CHARGE_TE_DEFAULT;
    return instance;
}

void subghz_protocol_encoder_tesla_charge_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderTeslaCharge* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Generating an upload from data.
 * @param instance Pointer to a SubGhzProtocolEncoderTeslaCharge instance
 * @return true On success
 */
static bool
    subghz_protocol_encoder_tesla_charge_get_upload(SubGhzProtocolEncoderTeslaCharge* instance) {
    furi_assert(instance);

    const uint32_t te = instance->te ? instance->te : TESLA_CHARGE_TE_DEFAULT;
    size_t index = 0;
    const size_t cap = instance->encoder.size_upload;

    // --- Preamble: 12 short high/low cells + a trailing short high ---
    for(uint8_t i = 0; i < 12; i++) {
        if(index + 2 > cap) return false;
        instance->encoder.upload[index++] = level_duration_make(true, te);
        instance->encoder.upload[index++] = level_duration_make(false, te);
    }
    if(index + 2 > cap) return false;
    instance->encoder.upload[index++] = level_duration_make(true, te);
    // --- Sync gap: ~3*te low ---
    instance->encoder.upload[index++] = level_duration_make(false, te * 3);

    // --- Payload: biphase, MSB first ---
    // Rule (verified bit-exact against captures & the manchester decoder):
    //   first  half-bit level = !prev_second   (transition on every bit boundary)
    //   second half-bit level = !bit
    // Entering the payload the line is low (the sync gap), so prev_second = 0.
    uint8_t prev_second = 0;
    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        uint8_t bit = (instance->generic.data >> (i - 1)) & 1;
        bool first = (prev_second == 0); // !prev_second -> high when prev was low
        bool second = (bit == 0); // !bit

        if(index + 2 > cap) return false;
        if(first == second) {
            // level held for the whole cell -> single 2*te pulse
            instance->encoder.upload[index++] = level_duration_make(first, te * 2);
        } else {
            instance->encoder.upload[index++] = level_duration_make(first, te);
            instance->encoder.upload[index++] = level_duration_make(second, te);
        }
        prev_second = second ? 1 : 0;
    }

    // --- Inter-burst gap before the next repeat ---
    if(index + 1 > cap) return false;
    instance->encoder.upload[index++] = level_duration_make(false, te * 3);

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus subghz_protocol_encoder_tesla_charge_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderTeslaCharge* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_tesla_charge_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        // Optional TE (fixed protocol, default 400us)
        if(!flipper_format_read_uint32(flipper_format, "TE", (uint32_t*)&instance->te, 1)) {
            instance->te = TESLA_CHARGE_TE_DEFAULT;
        }
        // Optional value
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        if(!subghz_protocol_encoder_tesla_charge_get_upload(instance)) {
            instance->encoder.front = 0;
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_tesla_charge_stop(void* context) {
    SubGhzProtocolEncoderTeslaCharge* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
}

LevelDuration subghz_protocol_encoder_tesla_charge_yield(void* context) {
    SubGhzProtocolEncoderTeslaCharge* instance = context;

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

/* ---------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------ */

void* subghz_protocol_decoder_tesla_charge_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderTeslaCharge* instance =
        malloc(sizeof(SubGhzProtocolDecoderTeslaCharge));
    instance->base.protocol = &subghz_protocol_tesla_charge;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_state = ManchesterStateMid1;
    return instance;
}

void subghz_protocol_decoder_tesla_charge_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;
    free(instance);
}

void subghz_protocol_decoder_tesla_charge_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->manchester_state = ManchesterStateMid1;
}

// Classify a pulse into a Manchester event; returns ManchesterEventReset for
// gaps / out-of-tolerance durations.
static ManchesterEvent
    subghz_protocol_tesla_charge_event(bool level, uint32_t duration) {
    if(duration >= TESLA_CHARGE_GAP_MIN) {
        return ManchesterEventReset;
    }
    if(DURATION_DIFF(duration, subghz_protocol_tesla_charge_const.te_short) <
       subghz_protocol_tesla_charge_const.te_delta) {
        return level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    }
    if(DURATION_DIFF(duration, subghz_protocol_tesla_charge_const.te_long) <
       subghz_protocol_tesla_charge_const.te_delta) {
        return level ? ManchesterEventLongHigh : ManchesterEventLongLow;
    }
    return ManchesterEventReset;
}

void subghz_protocol_decoder_tesla_charge_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;

    ManchesterEvent event = subghz_protocol_tesla_charge_event(level, duration);

    if(event == ManchesterEventReset) {
        // End of a burst (or noise). If a complete, matching payload was just
        // collected, accept it. The preamble decodes to a short run that never
        // matches, so only the true 50-bit Tesla payload triggers.
        if((instance->decoder.decode_count_bit ==
            subghz_protocol_tesla_charge_const.min_count_bit_for_found) &&
           (instance->decoder.decode_data == TESLA_CHARGE_FIX_CODE)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->manchester_state = ManchesterStateMid1;
        return;
    }

    bool bit = false;
    if(manchester_advance(
           instance->manchester_state, event, &instance->manchester_state, &bit)) {
        instance->decoder.decode_data = (instance->decoder.decode_data << 1) | (bit ? 1 : 0);
        instance->decoder.decode_count_bit++;
        // Overflow / desync guard - restart collection but keep decoding
        if(instance->decoder.decode_count_bit > 64) {
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
    }
}

uint8_t subghz_protocol_decoder_tesla_charge_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_tesla_charge_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t te = TESLA_CHARGE_TE_DEFAULT;
        if(!flipper_format_write_uint32(flipper_format, "TE", &te, 1)) {
            FURI_LOG_E(TAG, "Unable to add TE");
            ret = SubGhzProtocolStatusErrorParserTe;
        }
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_tesla_charge_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_tesla_charge_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_tesla_charge_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderTeslaCharge* instance = context;

    // push protocol data to global variable
    subghz_block_generic_global.btn_is_available = false;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%05lX%08lX\r\n"
        "Tesla Charge Port\r\n"
        "315(US)/433.92(EU) AM\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data >> 32) & 0xFFFFF,
        (uint32_t)(instance->generic.data & 0xFFFFFFFF));
}
