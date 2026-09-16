// [HITAG2_BF] Embedded flash dictionary of common Fiat/Alfa/Lancia BCM Hitag2 keys.
// 89 curated 6-byte keys (MSB-first, as fiat_v1_bcm_generate_authenticator consumes
// them): ASCII BCM-vendor patterns, low-entropy factory states, Proxmark HITAG2
// defaults, mutations of the 8 fiat_v1_known_keys, car-industry patterns, and
// anonymised Fiat community dumps. Ported from D4C1-Labs/Flipper-ARF; used by the
// HITAG2 Crack scene fast dictionary sweep (L2) before the slow Hell math (L5).
#pragma once
#include <stdint.h>
#include <stddef.h> // NULL

#ifdef __cplusplus
extern "C" {
#endif

// Number of keys in the embedded flash dictionary.
uint32_t subghz_hitag2_bf_flash_dict_size(void);

// Pointer to key[index] (6 bytes, MSB-first), or NULL if index is out of range.
const uint8_t (*subghz_hitag2_bf_flash_dict_get(uint32_t index))[6];

#ifdef __cplusplus
}
#endif
