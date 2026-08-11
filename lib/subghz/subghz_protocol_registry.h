#pragma once

#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const SubGhzProtocolRegistry subghz_protocol_registry;

// On-demand automotive protocol set (ProtoPirate car decoders), merged into a
// single resident set.
bool subghz_protocol_registry_automotive_mode(void); // true if any car mode is active
// Car mode: 0 = general (Off), 1 = Cars (single merged automotive set).
uint8_t subghz_protocol_registry_car_mode(void);
void subghz_protocol_registry_set_car_mode(uint8_t mode);
const SubGhzProtocolRegistry* subghz_protocol_registry_active(void);

typedef struct SubGhzProtocolDecoderBinRAW SubGhzProtocolDecoderBinRAW;

#ifdef __cplusplus
}
#endif
