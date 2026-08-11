#pragma once
#include <gui/view.h>

// Protocol Fuzzer run view (PRIVATE builds). Renders the live sweep state on
// the 128x64 display: frequency, mode/protocol, frame index/total, mutation
// descriptor, and a TX-active blink box. The scene (Task 8) pushes fresh
// state in via subghz_view_fuzzer_set_data() as the fuzzer_worker advances,
// and receives control-key intent back via subghz_view_fuzzer_set_callback().

typedef struct SubGhzViewFuzzer SubGhzViewFuzzer;

// Control events fired by the run view's input callback. The scene owning
// this view (Task 8) is expected to translate these into fuzzer_worker calls:
//   StartPause -> fuzzer_worker_start() / fuzzer_worker_pause(toggle)
//   Step       -> fuzzer_worker_step()
//   Mark       -> fuzzer_worker_mark_last()
//   Stop       -> fuzzer_worker_stop()
typedef enum {
    SubGhzViewFuzzerEventStartPause,
    SubGhzViewFuzzerEventStep,
    SubGhzViewFuzzerEventMark,
    SubGhzViewFuzzerEventStop,
} SubGhzViewFuzzerEvent;

typedef void (*SubGhzViewFuzzerCallback)(SubGhzViewFuzzerEvent event, void* context);

SubGhzViewFuzzer* subghz_view_fuzzer_alloc(void);
void subghz_view_fuzzer_free(SubGhzViewFuzzer* instance);
View* subghz_view_fuzzer_get_view(SubGhzViewFuzzer* instance);

void subghz_view_fuzzer_set_callback(
    SubGhzViewFuzzer* instance,
    SubGhzViewFuzzerCallback callback,
    void* context);

// Push fresh sweep state for the next draw. `mode_str` is the mode/protocol
// label for line 2 (e.g. "Protocol: KeeLoq FieldWalk"); `mutation_str` is the
// mutation descriptor for line 4 (e.g. "bit 42 flip"). Both are copied into
// the view model (bounded), so the caller's buffers need not outlive the call.
void subghz_view_fuzzer_set_data(
    SubGhzViewFuzzer* instance,
    uint32_t freq,
    const char* mode_str,
    const char* mutation_str,
    uint32_t index,
    uint32_t total,
    bool tx_on);
