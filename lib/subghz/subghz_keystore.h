#pragma once

#include <furi.h>
#include <m-array.h>
#include <stdint.h>
#include <esp_heap_caps.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* name; /* PSRAM-allocated (subghz_keystore.c) to keep key names off internal DRAM */
    uint64_t key;
    uint16_t type;
} SubGhzKey;

/* Route the keystore array's BACKING block (grows to 512-cap * sizeof(SubGhzKey) ~=
 * one ~12KB contiguous alloc) to PSRAM instead of internal DRAM. m-lib takes the
 * array-backing realloc/free from the ELEMENT oplist, so overriding REALLOC/FREE here
 * moves ONLY that backing block. Keys are read from the worker thread (subghz_receiver
 * decode), never an ISR, so PSRAM is safe. Frees ~12KB internal on the T-Embed so the
 * SubGHz app (40 presets + views) doesn't exhaust internal RAM and crash. */
/* Prefer PSRAM for the array backing; on no-PSRAM boards (M5Stick/Cardputer) SPIRAM
 * returns NULL, so fall back to internal so the array still grows as it did before.
 * heap_caps_realloc leaves the original block untouched when it returns NULL. */
static inline void* subghz_keystore_realloc_any(void* ptr, size_t bytes) {
    void* p = heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!p && bytes) p = heap_caps_realloc(ptr, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}
#define SUBGHZ_KEYSTORE_REALLOC(type, ptr, n) \
    ((type*)subghz_keystore_realloc_any((ptr), (n) * sizeof(type)))
#define SUBGHZ_KEYSTORE_FREE(ptr) free(ptr)
#define SUBGHZ_KEY_OPLIST \
    M_OPEXTEND(M_POD_OPLIST, REALLOC(SUBGHZ_KEYSTORE_REALLOC), FREE(SUBGHZ_KEYSTORE_FREE))

ARRAY_DEF(SubGhzKeyArray, SubGhzKey, SUBGHZ_KEY_OPLIST) //-V658

#define M_OPL_SubGhzKeyArray_t() ARRAY_OPLIST(SubGhzKeyArray, SUBGHZ_KEY_OPLIST)

typedef struct SubGhzKeystore SubGhzKeystore;

/**
 * Allocate SubGhzKeystore.
 * @return SubGhzKeystore* pointer to a SubGhzKeystore instance
 */
SubGhzKeystore* subghz_keystore_alloc(void);

/**
 * Free SubGhzKeystore.
 * @param instance Pointer to a SubGhzKeystore instance
 */
void subghz_keystore_free(SubGhzKeystore* instance);

/** 
 * Loading manufacture key from file
 * @param instance Pointer to a SubGhzKeystore instance
 * @param filename Full path to the file
 */
bool subghz_keystore_load(SubGhzKeystore* instance, const char* filename);

/** 
 * Save manufacture key to file
 * @param instance Pointer to a SubGhzKeystore instance
 * @param filename Full path to the file
 * @return true On success
 */
bool subghz_keystore_save(SubGhzKeystore* instance, const char* filename, uint8_t* iv);

/** 
 * Get array of keys and names manufacture
 * @param instance Pointer to a SubGhzKeystore instance
 * @return SubGhzKeyArray_t*
 */
SubGhzKeyArray_t* subghz_keystore_get_data(SubGhzKeystore* instance);

/** 
 * Save RAW encrypted to file
 * @param input_file_name Full path to the input file
 * @param output_file_name Full path to the output file
 * @param iv IV, 16 bytes in hex
 */
bool subghz_keystore_raw_encrypted_save(
    const char* input_file_name,
    const char* output_file_name,
    uint8_t* iv);

/** 
 * Get decrypt RAW data to file
 * @param file_name Full path to the input file
 * @param offset Offset from the start of the RAW data
 * @param data Returned array
 * @param len Required data length
 * @return true On success
 */
bool subghz_keystore_raw_get_data(const char* file_name, size_t offset, uint8_t* data, size_t len);

void subghz_keystore_reset_kl(SubGhzKeystore* instance);

#ifdef __cplusplus
}
#endif
