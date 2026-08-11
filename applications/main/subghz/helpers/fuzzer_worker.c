#include "fuzzer_worker.h"
#include "fuzzer_frame.h"
#include "subbrute_protocols.h" // subbrute_protocol_convert_preset / subbrute_protocol_preset (reused, not duplicated)

#include <furi.h>
#include <string.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <flipper_format.h>
#include <flipper_format_i.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/transmitter.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/blocks/custom_btn.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

#define TAG                "FuzzerWorker"
#define FUZZER_TX_TIMEOUT  6 // ms poll granularity while waiting on async TX completion
#define FUZZER_FRAME_DELAY 6 // ms between frames in the auto-sweep thread

// Protocol mode: "Protocol: %s\nBit: %u\nKey: 8x\"XX \"\nTE: %lu\nRepeat: %u\n" (<200B).
// RAW mode: "Protocol: RAW\nRAW_Data:" + up to FUZZ_MAX_DUR durations ("-32768 " = 7B each).
// Sized with margin above the RAW worst case (~1820B).
#define FUZZER_TX_BUF_SIZE (FUZZ_MAX_DUR * 7 + 128)

struct FuzzerWorker {
    FuriThread* thread;

    volatile bool worker_running;
    volatile bool paused;
    volatile bool transmit_mode;

    FuzzConfig cfg;
    uint32_t total;
    volatile uint32_t index;

    char protocol_name[32];
    uint32_t frequency;
    FuriHalSubGhzPreset preset;
    uint32_t te;
    uint8_t repeat;

    SubGhzEnvironment* environment;
    SubGhzTransmitter* transmitter;
    const SubGhzDevice* radio_device;
    bool radio_ready; // radio already configured for the current sweep/step

    FuzzFrame last_frame;
    bool has_last_frame;
    char tx_buf[FUZZER_TX_BUF_SIZE]; // scratch for fuzzer_frame_build(); heap (struct member), not stack

    FuzzerWorkerCallback callback;
    void* context;
};

static void fuzzer_worker_send_callback(FuzzerWorker* instance, uint32_t index) {
    if(instance->callback != NULL) {
        instance->callback(instance->context, index, instance->total);
    }
}

// Cloned from subbrute_worker_subghz_transmit(): deserialize the FlipperFormat
// into a SubGhzTransmitter and drive it over start_async_tx, exactly as
// SubBrute/Transmitter do. This is the on-air path; do not diverge from it.
static void fuzzer_worker_subghz_transmit(FuzzerWorker* instance, FlipperFormat* flipper_format) {
    const uint8_t timeout = FUZZER_TX_TIMEOUT;
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
        FURI_LOG_E(TAG, "Deserialize '%s' failed: %d", instance->protocol_name, deser_status);
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
        instance->transmit_mode = false;
        return;
    }

    // Freq/preset are invariant across a sweep: configure the radio once
    // (mirrors subbrute's fast_mode configure-once), not per frame.
    if(!instance->radio_ready) {
        subghz_devices_reset(instance->radio_device);
        subghz_devices_idle(instance->radio_device);
        subghz_devices_load_preset(instance->radio_device, instance->preset, NULL);
        subghz_devices_set_frequency(instance->radio_device, instance->frequency);
        instance->radio_ready = true;
    }

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

// Mutate -> build -> validate freq -> load into a string-backed FlipperFormat ->
// transmit. Stores the frame in instance->last_frame on success (for mark_last()).
static bool fuzzer_worker_transmit_index(FuzzerWorker* instance, uint32_t index) {
    if(!fuzzer_mutate_frame(&instance->cfg, index, &instance->last_frame)) {
        FURI_LOG_W(TAG, "mutate_frame failed at index %lu", (unsigned long)index);
        return false;
    }

    int n = fuzzer_frame_build(
        &instance->last_frame,
        instance->cfg.mode,
        instance->protocol_name,
        instance->te,
        instance->repeat,
        instance->tx_buf,
        sizeof(instance->tx_buf));
    if(n <= 0) {
        FURI_LOG_E(TAG, "frame_build failed/truncated at index %lu", (unsigned long)index);
        return false;
    }

    // Same freq-validity gate SubBrute/Transmitter use before arming TX
    // (subghz_devices_is_frequency_valid -> subbrute_worker_is_tx_allowed,
    // subghz_txrx_radio_device_is_frequency_valid, subghz_cli.c). Frequency is
    // invariant per sweep, but re-checked every frame so a bad config can never
    // slip a single frame onto the air.
    if(!subghz_devices_is_frequency_valid(instance->radio_device, instance->frequency)) {
        FURI_LOG_E(
            TAG,
            "freq %lu invalid for this device; TX blocked",
            (unsigned long)instance->frequency);
        return false;
    }

    FlipperFormat* ff = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(ff);
    stream_clean(stream);
    stream_write_cstring(stream, instance->tx_buf);
    stream_rewind(stream);

    fuzzer_worker_subghz_transmit(instance, ff);

    flipper_format_free(ff);
    instance->has_last_frame = true;
    return true;
}

static int32_t fuzzer_worker_thread(void* context) {
    furi_assert(context);
    FuzzerWorker* instance = context;

    instance->radio_ready = false; // configure the radio once at sweep start

    while(instance->worker_running) {
        if(instance->paused) {
            furi_delay_ms(20);
            continue;
        }
        if(instance->index >= instance->total) {
            break; // sweep finished
        }

        bool ok = fuzzer_worker_transmit_index(instance, instance->index);
        fuzzer_worker_send_callback(instance, instance->index);
        if(!ok) {
            // Frequency invalid / mutate or build failure: won't recover on the
            // next tick either, so stop the sweep instead of spinning.
            FURI_LOG_W(TAG, "Frame %lu failed; stopping sweep", (unsigned long)instance->index);
            break;
        }

        instance->index++;
        furi_delay_ms(FUZZER_FRAME_DELAY);
    }

    instance->worker_running = false;
    return 0;
}

FuzzerWorker* fuzzer_worker_alloc(void) {
    FuzzerWorker* instance = malloc(sizeof(FuzzerWorker));
    memset(instance, 0, sizeof(FuzzerWorker));

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "FuzzerWorker");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);
    furi_thread_set_callback(instance->thread, fuzzer_worker_thread);

    instance->environment = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)&subghz_protocol_registry);

    instance->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    instance->preset = FuriHalSubGhzPresetOok650Async; // sane default until set_config()
    strncpy(instance->protocol_name, "Princeton", sizeof(instance->protocol_name) - 1);

    return instance;
}

void fuzzer_worker_free(FuzzerWorker* instance) {
    furi_assert(instance);

    if(instance->worker_running) {
        fuzzer_worker_stop(instance);
    }

    if(instance->transmitter != NULL) {
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
    }

    subghz_environment_free(instance->environment);
    instance->environment = NULL;

    furi_thread_free(instance->thread);

    if(instance->radio_device != NULL) {
        subghz_devices_sleep(instance->radio_device);
    }

    free(instance);
}

void fuzzer_worker_set_config(
    FuzzerWorker* instance,
    const FuzzConfig* cfg,
    const char* protocol,
    uint32_t freq,
    const char* preset,
    uint32_t te,
    uint8_t repeat) {
    furi_assert(instance);
    furi_assert(cfg);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "set_config while running; stopping worker first");
        fuzzer_worker_stop(instance);
    }

    // Input-safety clamp (trust boundary): fuzzer_mutate_frame() writes
    // fixed-size FUZZ_MAX_BYTES/FUZZ_MAX_DUR arrays without its own bounds
    // checks (Tasks 1/3 gap flagged by review). Clamp here before the config
    // ever reaches fuzzer_mutate_count()/fuzzer_mutate_frame().
    FuzzConfig local = *cfg;
    const uint16_t max_bits = FUZZ_MAX_BYTES * 8;
    if(local.seed_nbits > max_bits) local.seed_nbits = max_bits;
    if(local.seed_ndur > FUZZ_MAX_DUR) local.seed_ndur = FUZZ_MAX_DUR;
    if(local.field_off > max_bits) local.field_off = max_bits;
    if((uint32_t)local.field_off + (uint32_t)local.field_len > max_bits) {
        local.field_len = max_bits - local.field_off;
    }
    instance->cfg = local;

    if(protocol != NULL) {
        strncpy(instance->protocol_name, protocol, sizeof(instance->protocol_name) - 1);
        instance->protocol_name[sizeof(instance->protocol_name) - 1] = '\0';
    }

    instance->frequency = freq;

    if(preset != NULL) {
        FuriString* preset_str = furi_string_alloc_set(preset);
        instance->preset = subbrute_protocol_convert_preset(preset_str);
        furi_string_free(preset_str);
    }

    instance->te = te;
    instance->repeat = repeat;

    instance->total = fuzzer_mutate_count(&instance->cfg);
    instance->index = 0;
    instance->paused = false;
    instance->radio_ready = false;
    instance->has_last_frame = false;
}

bool fuzzer_worker_start(FuzzerWorker* instance) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Worker is already running!");
        return false;
    }
    if(instance->total == 0) {
        FURI_LOG_W(TAG, "Worker not configured (0 frames)");
        return false;
    }
    if(!subghz_devices_is_frequency_valid(instance->radio_device, instance->frequency)) {
        FURI_LOG_E(
            TAG, "freq %lu invalid; refusing to start", (unsigned long)instance->frequency);
        return false;
    }

    instance->paused = false;
    instance->worker_running = true;
    furi_thread_start(instance->thread);

    return true;
}

void fuzzer_worker_stop(FuzzerWorker* instance) {
    furi_assert(instance);

    if(!instance->worker_running) {
        return;
    }

    instance->worker_running = false;
    furi_thread_join(instance->thread);

    subghz_devices_idle(instance->radio_device);
}

void fuzzer_worker_pause(FuzzerWorker* instance, bool paused) {
    furi_assert(instance);
    instance->paused = paused;
}

bool fuzzer_worker_step(FuzzerWorker* instance) {
    furi_assert(instance);

    if(instance->worker_running && !instance->paused) {
        FURI_LOG_W(TAG, "Cannot step while auto-running");
        return false;
    }
    if(instance->index >= instance->total) {
        return false;
    }
    if(!instance->worker_running) {
        instance->radio_ready = false; // standalone call always re-arms the radio
    }

    bool ok = fuzzer_worker_transmit_index(instance, instance->index);
    fuzzer_worker_send_callback(instance, instance->index);
    instance->index++;

    return ok;
}

bool fuzzer_worker_running(FuzzerWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

uint32_t fuzzer_worker_index(FuzzerWorker* instance) {
    furi_assert(instance);
    return instance->index;
}

void fuzzer_worker_set_callback(
    FuzzerWorker* instance,
    FuzzerWorkerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

bool fuzzer_worker_mark_last(FuzzerWorker* instance, const char* path) {
    furi_assert(instance);
    furi_assert(path);

    if(instance->worker_running && !instance->paused) {
        // Same guard as fuzzer_worker_step(): the sweep thread mutates
        // instance->last_frame/tx_buf every tick while running-and-unpaused, so
        // reading them here would race and could save a torn/mismatched .sub.
        FURI_LOG_W(TAG, "mark_last: cannot mark while auto-running (pause or stop first)");
        return false;
    }

    if(!instance->has_last_frame) {
        FURI_LOG_W(TAG, "mark_last: no frame sent yet");
        return false;
    }

    int n = fuzzer_frame_build(
        &instance->last_frame,
        instance->cfg.mode,
        instance->protocol_name,
        instance->te,
        instance->repeat,
        instance->tx_buf,
        sizeof(instance->tx_buf));
    if(n <= 0) {
        FURI_LOG_E(TAG, "mark_last: frame_build failed/truncated");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    bool result = false;
    do {
        if(!flipper_format_file_open_always(file, path)) {
            FURI_LOG_E(TAG, "mark_last: failed to open %s", path);
            break;
        }
        Stream* stream = flipper_format_get_raw_stream(file);
        stream_clean(stream);
        const char* filetype = (instance->cfg.mode == FuzzModeRaw) ?
                                    "Flipper SubGhz RAW File" :
                                    "Flipper SubGhz Key File";
        stream_write_format(
            stream,
            "Filetype: %s\nVersion: 1\nFrequency: %lu\nPreset: %s\n",
            filetype,
            (unsigned long)instance->frequency,
            subbrute_protocol_preset(instance->preset));
        stream_write_cstring(stream, instance->tx_buf);
        result = true;
    } while(false);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    return result;
}
