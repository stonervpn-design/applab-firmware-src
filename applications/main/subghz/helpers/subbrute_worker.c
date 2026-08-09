#include "subbrute_worker_private.h"
#include <furi.h>
#include <string.h>
#include <toolbox/stream/stream.h>
#include <flipper_format.h>
#include <flipper_format_i.h>
#include <lib/subghz/subghz_protocol_registry.h>
// use to clear custom_btn
#include <lib/subghz/blocks/custom_btn.h>
#include <toolbox/level_duration.h>
#include "subbrute_debruijn.h"
#ifndef PUBLIC_BUILD
#include "subbrute_mask.h"
#endif

#define TAG                               "SubBruteWorker"
#define SUBBRUTE_TX_TIMEOUT               6
#define SUBBRUTE_MANUAL_TRANSMIT_INTERVAL 250
#define SUBBRUTE_FAST_REPEAT              2 // repeat clamp in fast mode (receiver-latch floor)

// Defined lower down; forward-declared so the manual-transmit path can call it.
static void subbrute_worker_debruijn_transmit(SubBruteWorker* instance);

SubBruteWorker* subbrute_worker_alloc(const SubGhzDevice* radio_device) {
    SubBruteWorker* instance = malloc(sizeof(SubBruteWorker));

    instance->state = SubBruteWorkerStateIDLE;
    instance->step = 0;
    instance->worker_running = false;
    instance->initiated = false;
    instance->last_time_tx_data = 0;
    instance->load_index = 0;
    instance->opencode = 0;

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "SubBruteAttackWorker");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);
    furi_thread_set_callback(instance->thread, subbrute_worker_thread);

    instance->context = NULL;
    instance->callback = NULL;

    instance->tx_timeout_ms = SUBBRUTE_TX_TIMEOUT;
    instance->decoder_result = NULL;
    instance->transmitter = NULL;
    instance->environment = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)&subghz_protocol_registry);

    instance->transmit_mode = false;

    // Fast sweep defaults ON in private, OFF in public (public behavior unchanged).
#ifndef PUBLIC_BUILD
    instance->fast_mode = true;
#else
    instance->fast_mode = false;
#endif
    instance->radio_ready = false;

#ifndef PUBLIC_BUILD
    instance->mask_var = 0;
    instance->mask_active = false;
#endif

    instance->radio_device = radio_device;

    return instance;
}

void subbrute_worker_set_fast(SubBruteWorker* instance, bool fast) {
    furi_assert(instance);
    instance->fast_mode = fast;
}

bool subbrute_worker_is_fast(SubBruteWorker* instance) {
    furi_assert(instance);
    return instance->fast_mode;
}

#ifndef PUBLIC_BUILD
void subbrute_worker_set_func_mask(SubBruteWorker* instance, uint64_t var_mask, bool active) {
    furi_assert(instance);
    // A valid mask needs at least one and at most 63 variable bits; otherwise
    // disengage (byte-granular default). Applied at the next LoadFile init.
    if(active && subbrute_mask_max_value(var_mask) == 0) active = false;
    instance->mask_var = var_mask;
    instance->mask_active = active;
}
#endif

void subbrute_worker_free(SubBruteWorker* instance) {
    furi_assert(instance);

    // I don't know how to free this
    instance->decoder_result = NULL;

    if(instance->transmitter != NULL) {
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
    }

    subghz_environment_free(instance->environment);
    instance->environment = NULL;

    furi_thread_free(instance->thread);

    subghz_devices_sleep(instance->radio_device);
    subbrute_radio_device_loader_end(instance->radio_device);

    free(instance);
}

uint64_t subbrute_worker_get_step(SubBruteWorker* instance) {
    return instance->step;
}

bool subbrute_worker_set_step(SubBruteWorker* instance, uint64_t step) {
    furi_assert(instance);
    if(!subbrute_worker_can_manual_transmit(instance)) {
        FURI_LOG_W(TAG, "Cannot set step during running mode");

        return false;
    }

    instance->step = step;

    return true;
}

void subbrute_worker_set_opencode(SubBruteWorker* instance, uint8_t opencode) {
    // furi_assert(instance);
    // if(!subbrute_worker_can_manual_transmit(instance)) {
    //     FURI_LOG_W(TAG, "Cannot set opencode during running mode");
    //
    //     return false;
    // }
    instance->opencode = opencode;

    // return true;
}

uint8_t subbrute_worker_get_opencode(SubBruteWorker* instance) {
    return instance->opencode;
}

bool subbrute_worker_get_is_pt2262(SubBruteWorker* instance) {
    if(instance->attack == SubBruteAttackPT226224bit315 ||
       instance->attack == SubBruteAttackPT226224bit418 ||
       instance->attack == SubBruteAttackPT226224bit430 ||
       instance->attack == SubBruteAttackPT226224bit4305 ||
       instance->attack == SubBruteAttackPT226224bit433) {
        return true;
    } else {
        return false;
    }
}

bool subbrute_worker_init_default_attack(
    SubBruteWorker* instance,
    SubBruteAttacks attack_type,
    uint64_t step,
    const SubBruteProtocol* protocol,
    uint8_t repeats) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Init Worker when it's running");
        subbrute_worker_stop(instance);
    }

    instance->attack = attack_type;
    instance->frequency = protocol->frequency;
    instance->preset = protocol->preset;
    instance->file = protocol->file;
    instance->step = step;
    instance->bits = protocol->bits;
    instance->te = protocol->te;
    instance->opencode = protocol->opencode;
    instance->repeat = repeats;

    // For De Bruijn the per-row menu value is n (bits per code), not a repeat
    // count. Carry it into `bits`, clamped to the TX-safe range.
    if(attack_type == SubBruteAttackDeBruijn) {
        instance->bits = CLAMP(repeats, SUBBRUTE_DEBRUIJN_MAX_TX_N, SUBBRUTE_DEBRUIJN_MIN_TX_N);
    }
    instance->load_index = 0;
    instance->file_key = 0;
    instance->two_bytes = false;

    instance->max_value =
        subbrute_protocol_calc_max_value(instance->attack, instance->bits, instance->two_bytes);
#ifndef PUBLIC_BUILD
    // Function-mask brute is LoadFile-only; a preset attack never masks.
    instance->mask_active = false;
    subbrute_protocol_set_func_mask(0, false);
#endif

    instance->initiated = true;
    instance->state = SubBruteWorkerStateReady;
    subbrute_worker_send_callback(instance);
#ifdef FURI_DEBUG
    FURI_LOG_I(
        TAG,
        "subbrute_worker_init_default_attack: %s, bits: %d, preset: %s, file: %s, te: %ld, repeat: %d, max_value: %lld",
        subbrute_protocol_name(instance->attack),
        instance->bits,
        subbrute_protocol_preset(instance->preset),
        subbrute_protocol_file(instance->file),
        instance->te,
        instance->repeat,
        instance->max_value);
#endif

    return true;
}

bool subbrute_worker_init_file_attack(
    SubBruteWorker* instance,
    uint64_t step,
    uint8_t load_index,
    uint64_t file_key,
    SubBruteProtocol* protocol,
    uint8_t repeats,
    bool two_bytes) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Init Worker when it's running");
        subbrute_worker_stop(instance);
    }

    instance->attack = SubBruteAttackLoadFile;
    instance->frequency = protocol->frequency;
    instance->preset = protocol->preset;
    instance->file = protocol->file;
    instance->step = step;
    instance->bits = protocol->bits;
    instance->te = protocol->te;
    instance->load_index = load_index;
    instance->repeat = repeats;
    instance->file_key = file_key;
    instance->two_bytes = two_bytes;

#ifndef PUBLIC_BUILD
    if(instance->mask_active) {
        // Function-mask brute: enumerate only the k masked bits of file_key (2^k),
        // not the byte-granular space.
        instance->max_value = subbrute_mask_max_value(instance->mask_var);
        subbrute_protocol_set_func_mask(instance->mask_var, true);
    } else {
        instance->max_value = subbrute_protocol_calc_max_value(
            instance->attack, instance->bits, instance->two_bytes);
        subbrute_protocol_set_func_mask(0, false);
    }
#else
    instance->max_value =
        subbrute_protocol_calc_max_value(instance->attack, instance->bits, instance->two_bytes);
#endif

    instance->initiated = true;
    instance->state = SubBruteWorkerStateReady;
    subbrute_worker_send_callback(instance);
#ifdef FURI_DEBUG
    FURI_LOG_I(
        TAG,
        "subbrute_worker_init_file_attack: %s, bits: %d, preset: %s, file: %s, te: %ld, repeat: %d, max_value: %lld, key: %llX",
        subbrute_protocol_name(instance->attack),
        instance->bits,
        subbrute_protocol_preset(instance->preset),
        subbrute_protocol_file(instance->file),
        instance->te,
        instance->repeat,
        instance->max_value,
        instance->file_key);
#endif

    return true;
}

bool subbrute_worker_start(SubBruteWorker* instance) {
    furi_assert(instance);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Worker is already running!");
        return false;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Worker cannot start, invalid device state: %d", instance->state);
        return false;
    }

    instance->worker_running = true;
    furi_thread_start(instance->thread);

    return true;
}

void subbrute_worker_stop(SubBruteWorker* instance) {
    furi_assert(instance);

    if(!instance->worker_running) {
        return;
    }

    instance->worker_running = false;
    furi_thread_join(instance->thread);

    subghz_devices_idle(instance->radio_device);
}

bool subbrute_worker_transmit_current_key(SubBruteWorker* instance, uint64_t step) {
    furi_assert(instance);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }
    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Worker in running state!");
        return false;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Invalid state for running worker! State: %d", instance->state);
        return false;
    }

    uint32_t ticks = furi_get_tick();
    if((ticks - instance->last_time_tx_data) < SUBBRUTE_MANUAL_TRANSMIT_INTERVAL) {
#ifdef FURI_DEBUG
        FURI_LOG_D(TAG, "Need to wait, current: %ld", ticks - instance->last_time_tx_data);
#endif
        return false;
    }

    instance->last_time_tx_data = ticks;
    instance->step = step;

    if(instance->attack == SubBruteAttackDeBruijn) {
        subbrute_worker_debruijn_transmit(instance);
        return true;
    }

    bool result;
    instance->protocol_name = subbrute_protocol_file(instance->file);
    FlipperFormat* flipper_format = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(flipper_format);

    stream_clean(stream);

    if(instance->attack == SubBruteAttackLoadFile) {
        subbrute_protocol_file_payload(
            stream,
            step,
            instance->bits,
            instance->te,
            instance->repeat,
            instance->load_index,
            instance->file_key,
            instance->two_bytes);
    } else {
        subbrute_protocol_default_payload(
            stream,
            instance->file,
            step,
            instance->bits,
            instance->te,
            instance->repeat,
            instance->opencode);
    }

    instance->radio_ready = false; // manual single-shot always re-configures the radio
    subbrute_worker_subghz_transmit(instance, flipper_format);

    result = true;
#ifdef FURI_DEBUG
    FURI_LOG_W(TAG, "Manual transmit done");
#endif
    flipper_format_free(flipper_format);

    return result;
}

bool subbrute_worker_is_running(SubBruteWorker* instance) {
    return instance->worker_running;
}

bool subbrute_worker_can_manual_transmit(SubBruteWorker* instance) {
    furi_assert(instance);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }

    return !instance->worker_running && instance->state != SubBruteWorkerStateIDLE &&
           instance->state != SubBruteWorkerStateTx &&
           ((furi_get_tick() - instance->last_time_tx_data) > SUBBRUTE_MANUAL_TRANSMIT_INTERVAL);
}

void subbrute_worker_set_callback(
    SubBruteWorker* instance,
    SubBruteWorkerCallback callback,
    void* context) {
    furi_assert(instance);

    instance->callback = callback;
    instance->context = context;
}

// De Bruijn TX: the stock RAW encoder streams from a .sub file on disk, so we
// can't feed it an in-memory string. Instead we drive start_async_tx directly
// with our own yield over the 0/1 sequence, run-length merged into OOK durations.
typedef struct {
    const uint8_t* seq;
    size_t len;
    size_t pos;
    uint32_t te;
} SubBruteDebruijnTx;

static LevelDuration subbrute_worker_debruijn_yield(void* context) {
    SubBruteDebruijnTx* ctx = context;
    if(ctx->pos >= ctx->len) {
        return level_duration_reset();
    }
    uint8_t bit = ctx->seq[ctx->pos] & 1u;
    size_t run = 1;
    while(ctx->pos + run < ctx->len && (ctx->seq[ctx->pos + run] & 1u) == bit) {
        run++;
    }
    ctx->pos += run;
    return level_duration_make((bool)bit, (uint32_t)(run * ctx->te));
}

static void subbrute_worker_debruijn_transmit(SubBruteWorker* instance) {
    const uint8_t timeout = instance->tx_timeout_ms;
    while(instance->transmit_mode) {
        furi_delay_ms(timeout);
    }
    instance->transmit_mode = true;

    size_t seq_len = 0;
    uint8_t* seq = subbrute_debruijn_generate(instance->bits, &seq_len);
    if(seq == NULL) {
        FURI_LOG_E(TAG, "De Bruijn generate failed (n=%d)", instance->bits);
        instance->transmit_mode = false;
        return;
    }

    SubBruteDebruijnTx ctx = {
        .seq = seq,
        .len = seq_len,
        .pos = 0,
        .te = instance->te ? instance->te : 250,
    };

    subghz_devices_reset(instance->radio_device);
    subghz_devices_idle(instance->radio_device);
    subghz_devices_load_preset(instance->radio_device, instance->preset, NULL);
    subghz_devices_set_frequency(instance->radio_device, instance->frequency);

    bool tx_started = subghz_devices_start_async_tx(
        instance->radio_device, subbrute_worker_debruijn_yield, &ctx);
    if(tx_started) {
        while(!subghz_devices_is_async_complete_tx(instance->radio_device)) {
            furi_delay_ms(timeout);
        }
        subghz_devices_stop_async_tx(instance->radio_device);
    } else {
        FURI_LOG_E(
            TAG,
            "De Bruijn start_async_tx failed (freq=%lu preset=%d)",
            (unsigned long)instance->frequency,
            instance->preset);
    }

    subghz_devices_idle(instance->radio_device);
    free(seq);
    instance->transmit_mode = false;
}

void subbrute_worker_subghz_transmit(SubBruteWorker* instance, FlipperFormat* flipper_format) {
    // Fast mode trims the busy-poll granularity (completion is ~ms-quantised).
    const uint8_t timeout = instance->fast_mode ? 1 : instance->tx_timeout_ms;
    while(instance->transmit_mode) {
        furi_delay_ms(timeout);
    }
    instance->transmit_mode = true;
    if(instance->transmitter != NULL) {
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
    }

    instance->transmitter =
        subghz_transmitter_alloc_init(instance->environment, instance->protocol_name);
    if(instance->transmitter == NULL) {
        FURI_LOG_E(TAG, "Protocol '%s' not found in registry", instance->protocol_name);
        instance->transmit_mode = false;
        return;
    }

    SubGhzProtocolStatus deser_status =
        subghz_transmitter_deserialize(instance->transmitter, flipper_format);
    if(deser_status != SubGhzProtocolStatusOk) {
        FURI_LOG_E(
            TAG,
            "Deserialize '%s' failed: %d",
            instance->protocol_name,
            deser_status);
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
        instance->transmit_mode = false;
        return;
    }

    // Fast mode: freq/preset are invariant across a sweep, so configure the radio
    // ONCE (first code) instead of re-arming every code. radio_ready is reset per
    // sweep and before manual TX. Non-fast keeps the original per-code config.
    if(!instance->fast_mode || !instance->radio_ready) {
        subghz_devices_reset(instance->radio_device);
        subghz_devices_idle(instance->radio_device);
        subghz_devices_load_preset(instance->radio_device, instance->preset, NULL);
        subghz_devices_set_frequency(instance->radio_device, instance->frequency);
        instance->radio_ready = true;
    }

    // start_async_tx already drives CC1101 into TX state internally; no separate set_tx needed
    // (matches the working playlist TX path).
    bool tx_started = subghz_devices_start_async_tx(
        instance->radio_device, subghz_transmitter_yield, instance->transmitter);
    if(tx_started) {
        while(!subghz_devices_is_async_complete_tx(instance->radio_device)) {
            furi_delay_ms(timeout);
        }
        subghz_devices_stop_async_tx(instance->radio_device);
    } else {
        FURI_LOG_E(
            TAG,
            "start_async_tx failed (freq=%lu preset=%d)",
            (unsigned long)instance->frequency,
            instance->preset);
    }

    subghz_devices_idle(instance->radio_device);

    subghz_transmitter_stop(instance->transmitter);
    subghz_transmitter_free(instance->transmitter);
    instance->transmitter = NULL;

    instance->transmit_mode = false;

    Stream* stream = flipper_format_get_raw_stream(flipper_format);
    stream_rewind(stream);

    subghz_custom_btns_reset();
}

void subbrute_worker_send_callback(SubBruteWorker* instance) {
    if(instance->callback != NULL) {
        instance->callback(instance->context, instance->state);
    }
}

/**
 * Entrypoint for worker
 *
 * @param context SubBruteWorker*
 * @return 0 if ok
 */
int32_t subbrute_worker_thread(void* context) {
    furi_assert(context);
    SubBruteWorker* instance = (SubBruteWorker*)context;

    if(!instance->worker_running) {
        FURI_LOG_W(TAG, "Worker is not set to running state!");

        return -1;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Invalid state for running worker! State: %d", instance->state);

        return -2;
    }
#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "Worker start");
#endif

    SubBruteWorkerState local_state = instance->state = SubBruteWorkerStateTx;
    subbrute_worker_send_callback(instance);

    instance->protocol_name = subbrute_protocol_file(instance->file);

    FlipperFormat* flipper_format = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(flipper_format);

    instance->radio_ready = false; // configure the radio once at sweep start (fast mode)
    while(instance->worker_running) {
        if(instance->attack == SubBruteAttackDeBruijn) {
            // One continuous burst covers every code; transmit once and finish.
            subbrute_worker_debruijn_transmit(instance);
            local_state = SubBruteWorkerStateFinished;
            break;
        }
        stream_clean(stream);
        // Fast mode: clamp per-code repeats down to the receiver-latch floor.
        uint8_t rep = (instance->fast_mode && instance->repeat > SUBBRUTE_FAST_REPEAT) ?
                          SUBBRUTE_FAST_REPEAT :
                          instance->repeat;
        if(instance->attack == SubBruteAttackLoadFile) {
            subbrute_protocol_file_payload(
                stream,
                instance->step,
                instance->bits,
                instance->te,
                rep,
                instance->load_index,
                instance->file_key,
                instance->two_bytes);
        } else {
            subbrute_protocol_default_payload(
                stream,
                instance->file,
                instance->step,
                instance->bits,
                instance->te,
                rep,
                instance->opencode);
        }
#ifdef FURI_DEBUG
        //FURI_LOG_I(TAG, "Payload: %s", furi_string_get_cstr(payload));
        //furi_delay_ms(SUBBRUTE_MANUAL_TRANSMIT_INTERVAL / 4);
#endif

        subbrute_worker_subghz_transmit(instance, flipper_format);

        if(instance->step + 1 > instance->max_value) {
#ifdef FURI_DEBUG
            FURI_LOG_I(TAG, "Worker finished to end");
#endif
            local_state = SubBruteWorkerStateFinished;

            break;
        }
        instance->step++;

        furi_delay_ms(instance->fast_mode ? 1 : instance->tx_timeout_ms);
    }

    flipper_format_free(flipper_format);

    instance->worker_running = false; // Because we have error states
    instance->state = local_state == SubBruteWorkerStateTx ? SubBruteWorkerStateReady :
                                                             local_state;
    subbrute_worker_send_callback(instance);

#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "Worker stop");
#endif

    return 0;
}

uint8_t subbrute_worker_get_timeout(SubBruteWorker* instance) {
    return instance->tx_timeout_ms;
}

void subbrute_worker_set_timeout(SubBruteWorker* instance, uint8_t timeout) {
    instance->tx_timeout_ms = timeout;
}

uint8_t subbrute_worker_get_repeats(SubBruteWorker* instance) {
    return instance->repeat;
}

void subbrute_worker_set_repeats(SubBruteWorker* instance, uint8_t repeats) {
    instance->repeat = repeats;
}

uint32_t subbrute_worker_get_te(SubBruteWorker* instance) {
    return instance->te;
}

void subbrute_worker_set_te(SubBruteWorker* instance, uint32_t te) {
    instance->te = te;
}

bool subbrute_worker_is_tx_allowed(SubBruteWorker* instance, uint32_t value) {
    furi_assert(instance);
    bool res = false;

    if(!subghz_devices_is_frequency_valid(instance->radio_device, value)) {
        return false;
    } else {
        subghz_devices_set_frequency(instance->radio_device, value);
        res = subghz_devices_set_tx(instance->radio_device);
        subghz_devices_idle(instance->radio_device);
    }

    return res;
}

/*
void test_read_full_stream(Stream* stream, const char* msg) {
    // read data
    // 循环读取stream中的数据每次读取一个字节（uint8_t）
    size_t size_2 = stream_size(stream);
    // read data
    // 循环读取stream中的数据每次读取一个字节（uint8_t）
    char* data_2 = (char*)malloc(size_2 + 1);
    for(size_t i = 0; i < size_2; i++) {
        stream_read(stream, (uint8_t*)&data_2[i], 1);
    }
    data_2[size_2] = '\0';

    FURI_LOG_W(TAG, "%s Transmit data: %s", msg, data_2);
    free(data_2);
}
*/
