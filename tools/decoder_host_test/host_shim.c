// Host shims for the furi symbols the real decoder/encoder code links against.
// Lets the firmware's ProtoPirate + lib/subghz decode/encode code run on a PC.
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// Rolling-counter multiplier: real firmware default is 1 (feature disabled uses
// the sentinel -0x7FFFFFFF). Return 1 so keeloq/honda encoders advance the
// counter by the plain step, matching a stock device.
int32_t furi_hal_subghz_get_rolling_counter_mult(void) {
    return 1;
}

void __furi_crash_implementation(const char* message) {
    fprintf(stderr, "FURI CRASH: %s\n", message ? message : "(null)");
    abort();
}
void __furi_halt_implementation(const char* message) {
    fprintf(stderr, "FURI HALT: %s\n", message ? message : "(null)");
    abort();
}

// memmgr surface referenced by furi_string / flipper_format / blocks
size_t memmgr_get_free_heap(void) { return (size_t)1 << 20; }
size_t memmgr_get_total_heap(void) { return (size_t)1 << 21; }
size_t memmgr_get_minimum_free_heap(void) { return (size_t)1 << 20; }
size_t memmgr_pool_get_free(void) { return (size_t)1 << 20; }
size_t memmgr_pool_get_max_block(void) { return (size_t)1 << 20; }
void* aligned_malloc(size_t size, size_t alignment) {
    (void)alignment;
    return calloc(1, size);
}
void aligned_free(void* p) { free(p); }
void* memmgr_alloc_from_pool(size_t size) { return calloc(1, size); }

// ---- logging: no-op (decoders emit FURI_LOG_* we don't need on host) ----
void furi_log_print_format(int level, const char* tag, const char* format, ...) {
    (void)level;
    (void)tag;
    (void)format;
}
void furi_log_print_raw_format(int level, const char* format, ...) {
    (void)level;
    (void)format;
}

// ---- file streams: never used on host (we decode from in-memory RAW) ----
void* file_stream_alloc(void* storage) {
    (void)storage;
    return NULL;
}
void* buffered_file_stream_alloc(void* storage) {
    (void)storage;
    return NULL;
}
bool file_stream_open(void* s, const char* p, int am, int om) {
    (void)s;
    (void)p;
    (void)am;
    (void)om;
    return false;
}
bool buffered_file_stream_open(void* s, const char* p, int am, int om) {
    (void)s;
    (void)p;
    (void)am;
    (void)om;
    return false;
}
bool file_stream_close(void* s) {
    (void)s;
    return false;
}
bool buffered_file_stream_close(void* s) {
    (void)s;
    return false;
}
