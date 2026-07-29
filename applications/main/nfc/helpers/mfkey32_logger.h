#pragma once

#include <nfc/protocols/mf_classic/mf_classic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mfkey32Logger Mfkey32Logger;

/** One complete mfkey32 recovery input set (two same-key auths). */
typedef struct {
    uint32_t cuid;
    uint8_t sector_num;
    MfClassicKeyType key_type;
    uint32_t nt0;
    uint32_t nr0;
    uint32_t ar0;
    uint32_t nt1;
    uint32_t nr1;
    uint32_t ar1;
} Mfkey32Param;

Mfkey32Logger* mfkey32_logger_alloc(uint32_t cuid);

void mfkey32_logger_free(Mfkey32Logger* instance);

void mfkey32_logger_add_nonce(Mfkey32Logger* instance, MfClassicAuthContext* auth_context);

size_t mfkey32_logger_get_params_num(Mfkey32Logger* instance);

/** Copy out the i-th COMPLETE (filled) param set.
 *
 * @param instance  logger instance
 * @param index     index in [0, mfkey32_logger_get_params_num())
 * @param out       destination, filled on success
 * @return true if a filled param set exists at that index, false otherwise
 */
bool mfkey32_logger_get_param(Mfkey32Logger* instance, size_t index, Mfkey32Param* out);

bool mfkey32_logger_save_params(Mfkey32Logger* instance, const char* path);

void mfkey32_logger_get_params_data(Mfkey32Logger* instance, FuriString* str);

#ifdef __cplusplus
}
#endif
