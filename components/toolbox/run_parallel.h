#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback run by run_parallel(). Return value is ignored. */
typedef int32_t (*RunParallelCallback)(void* context);

/**
 * Run `callback(context)` once in a detached background thread, then let the
 * thread clean itself up. Fire-and-forget: the caller does not join or free.
 *
 * Port note: this fork's FuriThread has no detach, so this is implemented as a
 * self-deleting FreeRTOS task (stack_size is a byte count with a safe floor).
 */
void run_parallel(RunParallelCallback callback, void* context, uint32_t stack_size);

#ifdef __cplusplus
}
#endif
