#include "ble_walk_fuzz.h"

#ifndef PUBLIC_BUILD

#include "ble_adv_decode.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define TAG "BleWalkFuzz"

// Mode B (Stack) prelude: cycle the ATT MTU over these edge values before the
// write_char_long length sweep. 23 = the ATT spec floor (default MTU);
// 517 = the ATT spec ceiling; 0 and 0xFFFF are out-of-range values a
// well-behaved stack must clamp/reject rather than misbehave on.
static const uint16_t ble_walk_fuzz_mtu_edges[] = {23, 517, 0, 0xFFFF};
#define BLE_WALK_FUZZ_MTU_STEPS (sizeof(ble_walk_fuzz_mtu_edges) / sizeof(ble_walk_fuzz_mtu_edges[0]))

// Floor for the Sweep/Stack inter-iteration delay so rate_ms == 0 ("as fast
// as possible") still yields to the scheduler instead of busy-looping.
#define BLE_WALK_FUZZ_MIN_DELAY_MS 1
// DoS-mode poll interval cap: how often the engine thread re-reads the HAL's
// flood counter/fires the view callback, independent of the flood's own
// connect-cycle rate (which is entirely internal to ble_walk_hal's worker).
#define BLE_WALK_FUZZ_DOS_POLL_MS 20

struct BleWalkFuzz {
    FuriThread* thread;

    volatile bool worker_running;
    volatile bool paused;

    bool target_set;
    BleWalkDevice target; // owned copy, not a caller pointer

    BleFuzzMode mode;
    FuzzConfig cfg;
    uint16_t char_handle;
    bool write_norsp;
    uint32_t rate_ms;
    uint32_t count_cap; // Sweep/Stack: optional frame-count cap. DoS: optional connect-cycle cap.

    uint32_t total; // planned iterations for Sweep/Stack; 0 (unbounded) for DoS
    volatile uint32_t index;
    volatile uint32_t counter; // frames actually written (A/B) or completed connect cycles (C)

    // Last-iteration snapshot for mark_last(). Only touched by the engine
    // thread while running-and-unpaused; mark_last() itself only reads it
    // when stopped or paused (see the race-guard comment there), same
    // contract as fuzzer_worker's last_frame/has_last_frame.
    uint8_t last_payload[FUZZ_MAX_BYTES];
    uint16_t last_payload_len;
    uint16_t last_offset; // write_char_long offset used (Stack); 0 for Sweep/DoS
    FuzzStrategy last_strategy;
    uint32_t last_seed;
    uint32_t last_frame_index;
    bool has_last;

    BleWalkFuzzCallback callback;
    void* context;
};

static void ble_walk_fuzz_send_callback(BleWalkFuzz* instance) {
    if(instance->callback != NULL) {
        instance->callback(instance->context, instance->index, instance->counter);
    }
}

// FuzzFrame.bits is a fixed FUZZ_MAX_BYTES buffer written by fuzzer_mutate_frame()
// without its own bounds checks (same trust-boundary note as fuzzer_worker.c);
// nbits is caller-controlled via cfg.seed_nbits/field_off/field_len, so clamp
// the derived byte length defensively before it drives any array indexing or
// gets handed to the HAL as a length.
static uint16_t ble_walk_fuzz_frame_len(const FuzzFrame* frame) {
    uint16_t len = (uint16_t)((frame->nbits + 7) / 8);
    if(len > FUZZ_MAX_BYTES) len = FUZZ_MAX_BYTES;
    return len;
}

static void ble_walk_fuzz_snapshot_frame(BleWalkFuzz* instance, const FuzzFrame* frame, uint16_t len, uint16_t offset) {
    memcpy(instance->last_payload, frame->bits, len);
    instance->last_payload_len = len;
    instance->last_offset = offset;
    instance->last_strategy = frame->strategy;
    instance->last_seed = frame->seed;
    instance->last_frame_index = frame->index;
    instance->has_last = true;
}

// ---------------------------------------------------------------------------
// Mode A: Sweep -- fuzzer_mutate_frame() -> write_char[_norsp] against the
// caller's already-connected session.
// ---------------------------------------------------------------------------

static bool ble_walk_fuzz_sweep_step(BleWalkFuzz* instance, uint32_t index) {
    FuzzFrame frame;
    if(!fuzzer_mutate_frame(&instance->cfg, index, &frame)) {
        FURI_LOG_W(TAG, "sweep: mutate_frame failed at index %lu", (unsigned long)index);
        return false;
    }

    uint16_t len = ble_walk_fuzz_frame_len(&frame);
    bool ok = instance->write_norsp ?
                  ble_walk_hal_write_char_norsp(instance->char_handle, frame.bits, len) :
                  ble_walk_hal_write_char(instance->char_handle, frame.bits, len);

    ble_walk_fuzz_snapshot_frame(instance, &frame, len, 0);
    if(ok) instance->counter++;
    return ok;
}

// ---------------------------------------------------------------------------
// Mode B: Stack -- MTU edge-value cycle (BLE_WALK_FUZZ_MTU_STEPS iterations),
// then a write_char_long length sweep with offsets varied across 3 positions
// (start / mid-payload / past-end) to exercise the peer's prepare-write queue
// at different alignments, not just offset 0.
// ---------------------------------------------------------------------------

static bool ble_walk_fuzz_stack_step(BleWalkFuzz* instance, uint32_t index) {
    if(index < BLE_WALK_FUZZ_MTU_STEPS) {
        bool ok = ble_walk_hal_fuzz_mtu(ble_walk_fuzz_mtu_edges[index]);
        // No frame/payload for an MTU-only step; still record it as the last
        // "iteration" so mark_last has something coherent for this index
        // (last_payload left as whatever preceded it, len 0 signals "MTU step").
        instance->last_payload_len = 0;
        instance->last_offset = 0;
        instance->last_strategy = instance->cfg.strategy;
        instance->last_seed = instance->cfg.seed;
        instance->last_frame_index = index;
        instance->has_last = true;
        if(ok) instance->counter++;
        return ok;
    }

    uint32_t frame_index = index - BLE_WALK_FUZZ_MTU_STEPS;
    FuzzFrame frame;
    if(!fuzzer_mutate_frame(&instance->cfg, frame_index, &frame)) {
        FURI_LOG_W(TAG, "stack: mutate_frame failed at frame_index %lu", (unsigned long)frame_index);
        return false;
    }

    uint16_t len = ble_walk_fuzz_frame_len(&frame);
    uint16_t offset;
    switch(frame_index % 3) {
    case 0:
        offset = 0;
        break;
    case 1:
        offset = (uint16_t)(len / 2);
        break;
    default:
        offset = len;
        break;
    }

    bool ok = ble_walk_hal_write_char_long(instance->char_handle, offset, frame.bits, len);

    ble_walk_fuzz_snapshot_frame(instance, &frame, len, offset);
    if(ok) instance->counter++;
    return ok;
}

// ---------------------------------------------------------------------------
// Mode C: DoS -- the flood owns the connect/disconnect churn on its own HAL
// worker thread; this engine's thread just polls the counter and drives the
// callback/optional-cap logic. No per-cycle write: the flood's dwell window
// is too short and its connection state isn't ours to write against (see
// ble_walk_hal.h's single-connection-state note).
// ---------------------------------------------------------------------------

static int32_t ble_walk_fuzz_dos_thread(void* context) {
    BleWalkFuzz* instance = context;

    while(instance->worker_running) {
        if(!instance->paused) {
            instance->counter = ble_walk_hal_dos_counter();
            instance->index = instance->counter;
            ble_walk_fuzz_send_callback(instance);
            if(instance->count_cap != 0 && instance->counter >= instance->count_cap) {
                break;
            }
        }
        furi_delay_ms(BLE_WALK_FUZZ_DOS_POLL_MS);
    }

    ble_walk_hal_dos_flood_stop();
    instance->worker_running = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Modes A/B: shared thread body (single-frame-per-tick, rate-limited).
// ---------------------------------------------------------------------------

static int32_t ble_walk_fuzz_stimulus_thread(void* context) {
    BleWalkFuzz* instance = context;

    while(instance->worker_running) {
        if(instance->paused) {
            furi_delay_ms(20);
            continue;
        }
        if(instance->index >= instance->total) {
            break; // run finished
        }

        // Unlike the Sub-GHz fuzzer worker (where a failed frame means a
        // permanently-fatal build/frequency error), a single failed GATT
        // write here is expected fuzz noise -- peer momentarily busy, ATT
        // timeout, a malformed-on-purpose write getting rejected -- so the
        // sweep keeps advancing instead of aborting on the first failure.
        bool ok = (instance->mode == BleFuzzModeStack) ?
                      ble_walk_fuzz_stack_step(instance, instance->index) :
                      ble_walk_fuzz_sweep_step(instance, instance->index);
        (void)ok;
        ble_walk_fuzz_send_callback(instance);

        instance->index++;
        uint32_t delay = instance->rate_ms ? instance->rate_ms : BLE_WALK_FUZZ_MIN_DELAY_MS;
        furi_delay_ms(delay);
    }

    instance->worker_running = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

BleWalkFuzz* ble_walk_fuzz_alloc(void) {
    BleWalkFuzz* instance = malloc(sizeof(BleWalkFuzz));
    memset(instance, 0, sizeof(*instance));

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "BleWalkFuzz");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);
    // Callback is picked per-start() based on mode (DoS polls the HAL flood;
    // Sweep/Stack drive their own per-tick stimulus) -- see ble_walk_fuzz_start().
    furi_thread_set_callback(instance->thread, ble_walk_fuzz_stimulus_thread);

    return instance;
}

void ble_walk_fuzz_free(BleWalkFuzz* instance) {
    furi_assert(instance);

    if(instance->worker_running) {
        ble_walk_fuzz_stop(instance);
    }

    furi_thread_free(instance->thread);
    free(instance);
}

bool ble_walk_fuzz_set_target(BleWalkFuzz* instance, const BleWalkDevice* device) {
    furi_assert(instance);
    if(device == NULL) return false;

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "set_target while running; stopping first");
        ble_walk_fuzz_stop(instance);
    }

    instance->target = *device; // by-value copy: addr, addr_type, adv_data[], name[], ...
    instance->target_set = true;
    return true;
}

bool ble_walk_fuzz_config(
    BleWalkFuzz* instance,
    BleFuzzMode mode,
    FuzzConfig cfg,
    uint16_t char_handle,
    bool write_norsp,
    uint32_t rate_ms,
    uint32_t count) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "config while running; stopping first");
        ble_walk_fuzz_stop(instance);
    }

    instance->mode = mode;
    instance->cfg = cfg;
    instance->char_handle = char_handle;
    instance->write_norsp = write_norsp;
    instance->rate_ms = rate_ms;
    instance->count_cap = count;

    instance->index = 0;
    instance->counter = 0;
    instance->paused = false;
    instance->has_last = false;

    switch(mode) {
    case BleFuzzModeSweep: {
        uint32_t avail = fuzzer_mutate_count(&instance->cfg);
        instance->total = (count != 0 && count < avail) ? count : avail;
        break;
    }
    case BleFuzzModeStack: {
        // count caps only the length-sweep portion; the 4-step MTU-edge
        // prelude always runs in full.
        uint32_t avail = fuzzer_mutate_count(&instance->cfg);
        uint32_t sweep_total = (count != 0 && count < avail) ? count : avail;
        instance->total = (uint32_t)BLE_WALK_FUZZ_MTU_STEPS + sweep_total;
        break;
    }
    case BleFuzzModeDos:
    default:
        // Unbounded by design (runs until _stop()); count, if nonzero, is
        // reinterpreted as a connect-cycle cap polled by the DoS thread (see
        // the header doc on rate_ms/count for DoS).
        instance->total = 0;
        break;
    }

    return true;
}

bool ble_walk_fuzz_start(BleWalkFuzz* instance) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Already running (one mode at a time)");
        return false;
    }
    if(!instance->target_set) {
        FURI_LOG_W(TAG, "No target locked; refusing to start");
        return false;
    }
    if(instance->mode != BleFuzzModeDos && instance->char_handle == 0) {
        FURI_LOG_W(TAG, "No characteristic handle set; refusing to start");
        return false;
    }
    if((instance->mode == BleFuzzModeSweep || instance->mode == BleFuzzModeStack) &&
       !ble_walk_hal_is_connected()) {
        // Sweep/Stack write against the *current* HAL session; the caller
        // connects via ble_walk_hal_connect() to the locked target before
        // arming -- this engine never opens the connection itself for A/B.
        FURI_LOG_W(TAG, "No active connection; refusing to start (connect first)");
        return false;
    }
    if(instance->mode != BleFuzzModeDos && instance->total == 0) {
        FURI_LOG_W(TAG, "Nothing to run (0 planned iterations)");
        return false;
    }

    instance->index = 0;
    instance->counter = 0;
    instance->paused = false;

    if(instance->mode == BleFuzzModeDos) {
        // rate_ms doubles as the flood's per-connection dwell here, converted
        // to the HAL's 20ms/unit hold_count (0 == max churn) -- see the
        // header doc on ble_walk_fuzz_config()'s rate_ms parameter for why.
        uint16_t hold_count = (uint16_t)(instance->rate_ms / 20);
        if(!ble_walk_hal_dos_connect_flood_start(&instance->target, hold_count)) {
            FURI_LOG_E(TAG, "dos_connect_flood_start failed");
            return false;
        }
        furi_thread_set_callback(instance->thread, ble_walk_fuzz_dos_thread);
    } else {
        furi_thread_set_callback(instance->thread, ble_walk_fuzz_stimulus_thread);
    }

    instance->worker_running = true;
    furi_thread_start(instance->thread);
    return true;
}

void ble_walk_fuzz_stop(BleWalkFuzz* instance) {
    furi_assert(instance);

    if(!instance->worker_running) {
        return;
    }

    instance->worker_running = false;
    furi_thread_join(instance->thread);
    // The thread bodies also call ble_walk_hal_dos_flood_stop() on their own
    // exit path (self-terminated-via-count_cap case); calling it again here
    // for the externally-stopped case is idempotent per ble_walk_hal's contract.
    if(instance->mode == BleFuzzModeDos) {
        ble_walk_hal_dos_flood_stop();
    }
}

void ble_walk_fuzz_pause(BleWalkFuzz* instance, bool paused) {
    furi_assert(instance);
    instance->paused = paused;
}

bool ble_walk_fuzz_step(BleWalkFuzz* instance) {
    furi_assert(instance);

    if(instance->mode == BleFuzzModeDos) {
        FURI_LOG_W(TAG, "step: not supported for DoS mode (flood is HAL-thread-driven)");
        return false;
    }
    if(instance->worker_running && !instance->paused) {
        FURI_LOG_W(TAG, "Cannot step while auto-running");
        return false;
    }
    if(!instance->target_set || instance->char_handle == 0) {
        return false;
    }
    if(instance->index >= instance->total) {
        return false;
    }

    bool ok = (instance->mode == BleFuzzModeStack) ?
                  ble_walk_fuzz_stack_step(instance, instance->index) :
                  ble_walk_fuzz_sweep_step(instance, instance->index);
    ble_walk_fuzz_send_callback(instance);
    instance->index++;

    return ok;
}

bool ble_walk_fuzz_running(BleWalkFuzz* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

uint32_t ble_walk_fuzz_index(BleWalkFuzz* instance) {
    furi_assert(instance);
    return instance->index;
}

uint32_t ble_walk_fuzz_counter(BleWalkFuzz* instance) {
    furi_assert(instance);
    return instance->counter;
}

uint32_t ble_walk_fuzz_total(BleWalkFuzz* instance) {
    furi_assert(instance);
    return instance->total;
}

void ble_walk_fuzz_set_callback(BleWalkFuzz* instance, BleWalkFuzzCallback callback, void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

// ---------------------------------------------------------------------------
// Mark
// ---------------------------------------------------------------------------

static void mf_writef(File* file, const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    if(n > (int)sizeof(buf)) n = (int)sizeof(buf);
    storage_file_write(file, buf, (uint16_t)n);
}

static const char* ble_walk_fuzz_mode_name(BleFuzzMode mode) {
    switch(mode) {
    case BleFuzzModeSweep:
        return "Sweep";
    case BleFuzzModeStack:
        return "Stack";
    case BleFuzzModeDos:
        return "DoS";
    default:
        return "?";
    }
}

static const char* ble_walk_fuzz_strategy_name(FuzzStrategy strategy) {
    switch(strategy) {
    case FuzzStrategyBitFlipSweep:
        return "BitFlip";
    case FuzzStrategyBoundary:
        return "Boundary";
    case FuzzStrategyLength:
        return "Length";
    case FuzzStrategyRandom:
        return "Random";
    case FuzzStrategyFieldWalk:
        return "FieldWalk";
    default:
        return "?";
    }
}

bool ble_walk_fuzz_mark_last(BleWalkFuzz* instance, const char* path) {
    furi_assert(instance);
    furi_assert(path);

    if(!instance->target_set) {
        FURI_LOG_W(TAG, "mark_last: no target locked");
        return false;
    }
    if(instance->worker_running && !instance->paused) {
        // Same guard as fuzzer_worker_mark_last(): the engine thread mutates
        // last_payload/last_* every tick (and the DoS thread advances
        // index/counter) while running-and-unpaused, so reading them here
        // would race and could save a torn/mismatched report.
        FURI_LOG_W(TAG, "mark_last: cannot mark while auto-running (pause or stop first)");
        return false;
    }

    BleAdvInfo adv = {0};
    ble_adv_decode(instance->target.adv_data, instance->target.adv_data_len, &adv);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool result = false;

    do {
        if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            FURI_LOG_E(TAG, "mark_last: failed to open %s", path);
            break;
        }

        mf_writef(file, "BLE GATT Fuzz Mark\n");
        mf_writef(file, "===================\n");

        mf_writef(file, "[Target]\n");
        const uint8_t* a = instance->target.addr;
        mf_writef(
            file,
            "Address: %02X:%02X:%02X:%02X:%02X:%02X (type=%d)\n",
            a[0], a[1], a[2], a[3], a[4], a[5],
            (int)instance->target.addr_type);
        mf_writef(
            file,
            "Name: %s\n",
            instance->target.name[0] ? instance->target.name : "(unknown)");
        mf_writef(file, "RSSI: %d dBm\n", (int)instance->target.rssi);

        if(adv.has_flags) {
            mf_writef(file, "AdvFlags: 0x%02X\n", adv.flags);
        }
        if(adv.uuid16_count > 0) {
            mf_writef(file, "AdvUUID16:");
            for(uint8_t i = 0; i < adv.uuid16_count; i++) {
                mf_writef(file, " %04X", adv.uuid16[i]);
            }
            mf_writef(file, "\n");
        }
        if(adv.has_uuid128) {
            mf_writef(file, "AdvUUID128 (raw LE bytes):");
            for(uint8_t i = 0; i < 16; i++) {
                mf_writef(file, " %02X", adv.uuid128[i]);
            }
            mf_writef(file, "\n");
        }
        if(adv.has_tx_power) {
            mf_writef(file, "AdvTxPower: %d dBm\n", (int)adv.tx_power);
        }
        if(adv.mfr_present) {
            mf_writef(file, "AdvMfrID: 0x%04X\n", adv.mfr_id);
            if(adv.mfr_data_len > 0) {
                mf_writef(file, "AdvMfrData:");
                for(uint8_t i = 0; i < adv.mfr_data_len; i++) {
                    mf_writef(file, " %02X", adv.mfr_data[i]);
                }
                mf_writef(file, "\n");
            }
        }

        mf_writef(file, "\n[Run]\n");
        mf_writef(file, "Mode: %s\n", ble_walk_fuzz_mode_name(instance->mode));
        mf_writef(file, "CharHandle: 0x%04X\n", instance->char_handle);
        mf_writef(file, "WriteType: %s\n", instance->write_norsp ? "WithoutResponse" : "WithResponse");
        mf_writef(file, "NegotiatedMTU: %u\n", (unsigned)ble_walk_hal_get_negotiated_mtu());
        mf_writef(file, "Index: %lu\n", (unsigned long)instance->index);
        mf_writef(file, "Counter: %lu\n", (unsigned long)instance->counter);

        if(instance->mode == BleFuzzModeDos) {
            mf_writef(file, "DosConnectCycles: %lu\n", (unsigned long)ble_walk_hal_dos_counter());
        } else if(instance->has_last) {
            mf_writef(file, "Strategy: %s\n", ble_walk_fuzz_strategy_name(instance->last_strategy));
            mf_writef(file, "Seed: 0x%08lX\n", (unsigned long)instance->last_seed);
            mf_writef(file, "FrameIndex: %lu\n", (unsigned long)instance->last_frame_index);
            if(instance->mode == BleFuzzModeStack) {
                mf_writef(file, "Offset: %u\n", (unsigned)instance->last_offset);
            }

            mf_writef(file, "\n[Last Payload] (%u bytes)\n", (unsigned)instance->last_payload_len);
            if(instance->last_payload_len == 0) {
                mf_writef(file, "(MTU-only step; no payload)\n");
            } else {
                for(uint16_t i = 0; i < instance->last_payload_len; i++) {
                    mf_writef(file, "%02X ", instance->last_payload[i]);
                }
                mf_writef(file, "\n");
            }
        } else {
            mf_writef(file, "\n[Last Payload]\n(none sent yet)\n");
        }

        result = true;
    } while(false);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return result;
}

#endif // PUBLIC_BUILD
