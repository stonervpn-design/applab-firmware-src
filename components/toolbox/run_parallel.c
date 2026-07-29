#include <toolbox/run_parallel.h>

#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

typedef struct {
    RunParallelCallback callback;
    void* context;
} RunParallelData;

static void run_parallel_task(void* param) {
    RunParallelData* data = param;
    if(data->callback) data->callback(data->context);
    free(data);
    vTaskDelete(NULL); // reclaims this task's stack + TCB
}

void run_parallel(RunParallelCallback callback, void* context, uint32_t stack_size) {
    if(!callback) return;
    RunParallelData* data = malloc(sizeof(RunParallelData));
    if(!data) return;
    data->callback = callback;
    data->context = context;

    // ESP-IDF xTaskCreate stack depth is in BYTES; enforce a safe floor so short
    // Flipper stack requests (e.g. 2048) don't overflow doing record/storage work.
    uint32_t stack = stack_size < 4096 ? 4096 : stack_size;
    if(xTaskCreate(run_parallel_task, "RunParallel", stack, data, tskIDLE_PRIORITY + 5, NULL) !=
       pdPASS) {
        free(data);
    }
}
