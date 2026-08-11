#pragma once
#include <gui/view.h>

// BLE GATT Fuzzer/DoS run view (PRIVATE builds). Renders the live run state on
// the 128x64 display: target name, address, mode label, iteration index/total
// + counter, and a TX-active blink box. The scene (Task 6) pushes fresh state
// in via ble_fuzz_view_set_data() as the ble_walk_fuzz engine (Task 4)
// advances, and receives control-key intent back via
// ble_fuzz_view_set_callback(). Mirrors the Sub-GHz Protocol Fuzzer run view's
// (subghz_view_fuzzer.c) alloc/model/draw/input idiom.
#ifndef PUBLIC_BUILD

#include <stdint.h>
#include <stdbool.h>

typedef struct BleFuzzView BleFuzzView;

// Control events fired by the run view's input callback. The scene owning
// this view (Task 6) is expected to translate these into ble_walk_fuzz calls:
//   StartPause -> ble_walk_fuzz_start() / ble_walk_fuzz_pause(toggle)
//   Step       -> ble_walk_fuzz_step()
//   Mark       -> ble_walk_fuzz_mark_last()
//   Stop       -> ble_walk_fuzz_stop()
typedef enum {
    BleFuzzViewEventStartPause,
    BleFuzzViewEventStep,
    BleFuzzViewEventMark,
    BleFuzzViewEventStop,
} BleFuzzViewEvent;

typedef void (*BleFuzzViewCallback)(BleFuzzViewEvent event, void* context);

BleFuzzView* ble_fuzz_view_alloc(void);
void ble_fuzz_view_free(BleFuzzView* instance);
View* ble_fuzz_view_get_view(BleFuzzView* instance);

void ble_fuzz_view_set_callback(BleFuzzView* instance, BleFuzzViewCallback callback, void* context);

// Push fresh run state for the next draw. `target_name`, `addr_str`, and
// `mode_str` are copied into the view model (bounded strncpy into fixed
// buffers), so the caller's buffers need not outlive the call and no caller
// pointer is retained. `total` == 0 renders as unbounded ("N/--"), matching
// the DoS mode's unbounded-unless-capped run length.
void ble_fuzz_view_set_data(
    BleFuzzView* instance,
    const char* target_name,
    const char* addr_str,
    const char* mode_str,
    uint32_t index,
    uint32_t total,
    uint32_t counter,
    bool tx_on);

#endif // PUBLIC_BUILD
