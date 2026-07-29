#pragma once

#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const SubGhzProtocolRegistry subghz_protocol_registry;

// On-demand automotive protocol set (ProtoPirate car decoders). Split into two
// halves (Cars 1 / Cars 2) so the no-PSRAM board loads only ~32 decoders at once.
bool subghz_protocol_registry_automotive_mode(void); // true if any car mode is active
uint8_t subghz_protocol_registry_car_mode(void); // 0=general, 1=Cars 1, 2=Cars 2
void subghz_protocol_registry_set_car_mode(uint8_t mode);
const SubGhzProtocolRegistry* subghz_protocol_registry_active(void);

typedef struct SubGhzProtocolDecoderBinRAW SubGhzProtocolDecoderBinRAW;

#ifdef __cplusplus
}
#endif
