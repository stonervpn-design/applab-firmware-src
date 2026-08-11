// Minimal FreeRTOS stub so furi-core headers PARSE on host.
// We do not run the RTOS; the decoders never call these — headers just need types.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;

typedef void* TaskHandle_t;
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* StreamBufferHandle_t;
typedef void* MessageBufferHandle_t;
typedef void* EventGroupHandle_t;
typedef void* TimerHandle_t;
typedef void* QueueSetHandle_t;
typedef void* QueueSetMemberHandle_t;

typedef struct {
    void* dummy;
} StaticSemaphore_t;
typedef struct {
    void* dummy;
} StaticQueue_t;
typedef struct {
    void* dummy;
} StaticEventGroup_t;
typedef struct {
    void* dummy;
} StaticStreamBuffer_t;
typedef struct {
    void* dummy;
} StaticTimer_t;
typedef struct {
    void* dummy;
} StaticTask_t;

#ifndef portMAX_DELAY
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#endif
#ifndef pdTRUE
#define pdTRUE ((BaseType_t)1)
#endif
#ifndef pdFALSE
#define pdFALSE ((BaseType_t)0)
#endif
#ifndef pdPASS
#define pdPASS ((BaseType_t)1)
#endif
#ifndef pdFAIL
#define pdFAIL ((BaseType_t)0)
#endif
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif
#ifndef configMINIMAL_STACK_SIZE
#define configMINIMAL_STACK_SIZE 128
#endif

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))

static inline BaseType_t xPortInIsrContext(void) {
    return 0;
}
static inline BaseType_t xPortIsInsideInterrupt(void) {
    return 0;
}
