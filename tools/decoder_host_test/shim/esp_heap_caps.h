// Host stub for ESP-IDF's <esp_heap_caps.h>. The firmware routes the KeeLoq keystore
// backing array + key names + preset blobs to PSRAM via heap_caps_* (see subghz_keystore.h
// / subghz_setting.c); on the host there is no PSRAM, so every capability maps to plain
// libc allocation. shim/ is first on the harness -I path, so this wins over the real header.
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_EXEC     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_DEFAULT  (1 << 12)

static inline void* heap_caps_malloc(size_t size, unsigned int caps) {
    (void)caps;
    return malloc(size);
}
static inline void* heap_caps_calloc(size_t n, size_t size, unsigned int caps) {
    (void)caps;
    return calloc(n, size);
}
static inline void* heap_caps_realloc(void* ptr, size_t size, unsigned int caps) {
    (void)caps;
    return realloc(ptr, size);
}
static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned int caps) {
    (void)alignment;
    (void)caps;
    return malloc(size);
}
static inline void heap_caps_free(void* ptr) {
    free(ptr);
}
static inline size_t heap_caps_get_free_size(unsigned int caps) {
    (void)caps;
    return (size_t)1 << 20;
}
static inline size_t heap_caps_get_largest_free_block(unsigned int caps) {
    (void)caps;
    return (size_t)1 << 20;
}
