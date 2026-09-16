#include "subghz_worker.h"

#include <furi.h>
#include <esp_heap_caps.h>

#define TAG "SubGhzWorker"

/* Cap / floor for the adaptive capture buffer (entries of LevelDuration). */
#define SUBGHZ_WORKER_BUF_MAX_ENTRIES 4096u
#define SUBGHZ_WORKER_BUF_MIN_ENTRIES 512u
/* Internal-RAM headroom left free after sizing the capture buffer. Must cover
 * everything ELSE that subghz_alloc() allocates AFTER the worker (the SubBrute
 * device/worker/settings/views, the receiver view, scene manager, etc.) AND the
 * later file-browser open (SubGhz->Saved), whose BrowserWorker needs a ~3 KB
 * INTERNAL stack — on this maxed board 24 KB left <3 KB by then, so the browser
 * worker's stack spilled to PSRAM (which FreeRTOS rejects) and rebooted. 30 KB
 * shrinks the (largest) RX capture buffer a little to keep that headroom for the
 * browser + the raised Desktop stack. The RX buffer stays >= 512-entry floor. */
#define SUBGHZ_WORKER_BUF_HEADROOM (30u * 1024u)

struct SubGhzWorker {
    FuriThread* thread;
    FuriStreamBuffer* stream;

    volatile bool running;
    volatile bool overrun;

    LevelDuration filter_level_duration;
    uint16_t filter_duration;

    SubGhzWorkerOverrunCallback overrun_callback;
    SubGhzWorkerPairCallback pair_callback;
    void* context;
};

/** Rx callback timer
 * 
 * @param level received signal level
 * @param duration received signal duration
 * @param context 
 */
void subghz_worker_rx_callback(bool level, uint32_t duration, void* context) {
    SubGhzWorker* instance = context;

    LevelDuration level_duration = level_duration_make(level, duration);
    if(instance->overrun) {
        instance->overrun = false;
        level_duration = level_duration_reset();
    }
    size_t ret =
        furi_stream_buffer_send(instance->stream, &level_duration, sizeof(LevelDuration), 0);
    if(sizeof(LevelDuration) != ret) instance->overrun = true;
}

/** Worker callback thread
 * 
 * @param context 
 * @return exit code 
 */
static int32_t subghz_worker_thread_callback(void* context) {
    SubGhzWorker* instance = context;

    LevelDuration level_duration;

    while(instance->running) {
        int ret = furi_stream_buffer_receive(
            instance->stream, &level_duration, sizeof(LevelDuration), 10);
        if(ret == sizeof(LevelDuration)) {
            if(level_duration_is_reset(level_duration)) {
                if(instance->overrun_callback) instance->overrun_callback(instance->context);
            } else {
                bool level = level_duration_get_level(level_duration);
                uint32_t duration = level_duration_get_duration(level_duration);

                if((duration < instance->filter_duration) ||
                   (instance->filter_level_duration.level == level)) {
                    instance->filter_level_duration.duration += duration;

                } else if(instance->filter_level_duration.level != level) {
                    if(instance->pair_callback)
                        instance->pair_callback(
                            instance->context,
                            instance->filter_level_duration.level,
                            instance->filter_level_duration.duration);

                    instance->filter_level_duration.duration = duration;
                    instance->filter_level_duration.level = level;
                }
            }
        }
    }

    return 0;
}

/* Adaptive capture-buffer sizing + alloc. Shared by subghz_worker_alloc() and
 * subghz_worker_buffer_reacquire(). A fixed 16 KB buffer OOM-reboots on this board once
 * BLE + the full decoder set have eaten internal RAM; cap at 4096 entries but shrink to
 * the largest free internal block minus headroom, with a 512-entry floor. */
static void subghz_worker_stream_alloc(SubGhzWorker* instance) {
    size_t entries = SUBGHZ_WORKER_BUF_MAX_ENTRIES;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t budget = (largest > SUBGHZ_WORKER_BUF_HEADROOM) ?
                        (largest - SUBGHZ_WORKER_BUF_HEADROOM) :
                        0;
    size_t fit_entries = budget / sizeof(LevelDuration);
    if(fit_entries < entries) entries = fit_entries;
    if(entries < SUBGHZ_WORKER_BUF_MIN_ENTRIES) entries = SUBGHZ_WORKER_BUF_MIN_ENTRIES;
    FURI_LOG_I(
        TAG,
        "capture buffer: %u entries (%u B), largest free internal=%u B",
        (unsigned)entries,
        (unsigned)(entries * sizeof(LevelDuration)),
        (unsigned)largest);
    instance->stream =
        furi_stream_buffer_alloc(sizeof(LevelDuration) * entries, sizeof(LevelDuration));
}

SubGhzWorker* subghz_worker_alloc(void) {
    SubGhzWorker* instance = calloc(1, sizeof(SubGhzWorker));

    instance->thread =
        furi_thread_alloc_ex("SubGhzWorker", 3072, subghz_worker_thread_callback, instance);

    subghz_worker_stream_alloc(instance);

    //setting default filter in us
    instance->filter_duration = 30;

    return instance;
}

/* Temporarily release the capture buffer (~4 KB internal) while it is not needed — e.g.
 * the SubGhz->Saved file browser is open and internal DRAM is exhausted, so the browser's
 * own worker thread cannot get an internal stack. Only safe while the worker is STOPPED
 * (the decode thread reads instance->stream). subghz_worker_buffer_reacquire() restores it. */
void subghz_worker_buffer_release(SubGhzWorker* instance) {
    furi_check(instance);
    if(instance->running) return; // never free a buffer the decode thread is reading
    if(instance->stream) {
        furi_stream_buffer_free(instance->stream);
        instance->stream = NULL;
    }
}

void subghz_worker_buffer_reacquire(SubGhzWorker* instance) {
    furi_check(instance);
    if(!instance->stream) subghz_worker_stream_alloc(instance);
}

void subghz_worker_free(SubGhzWorker* instance) {
    furi_check(instance);

    if(instance->stream) furi_stream_buffer_free(instance->stream);
    furi_thread_free(instance->thread);

    free(instance);
}

void subghz_worker_set_overrun_callback(
    SubGhzWorker* instance,
    SubGhzWorkerOverrunCallback callback) {
    furi_check(instance);
    instance->overrun_callback = callback;
}

void subghz_worker_set_pair_callback(SubGhzWorker* instance, SubGhzWorkerPairCallback callback) {
    furi_check(instance);
    instance->pair_callback = callback;
}

void subghz_worker_set_context(SubGhzWorker* instance, void* context) {
    furi_check(instance);
    instance->context = context;
}

void subghz_worker_start(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(!instance->running);

    instance->running = true;

    furi_thread_start(instance->thread);
}

void subghz_worker_stop(SubGhzWorker* instance) {
    furi_check(instance);
    furi_check(instance->running);

    instance->running = false;

    furi_thread_join(instance->thread);
}

bool subghz_worker_is_running(SubGhzWorker* instance) {
    furi_check(instance);
    return instance->running;
}

void subghz_worker_set_filter(SubGhzWorker* instance, uint16_t timeout) {
    furi_check(instance);
    instance->filter_duration = timeout;
}
