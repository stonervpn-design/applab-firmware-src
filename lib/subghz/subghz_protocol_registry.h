#pragma once

#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const SubGhzProtocolRegistry subghz_protocol_registry;

// On-demand automotive protocol set (ProtoPirate car decoders). Kept out of the
// default registry so the no-PSRAM board doesn't load all decoders at once.
extern const SubGhzProtocolRegistry subghz_protocol_registry_automotive;

bool subghz_protocol_registry_automotive_mode(void);
void subghz_protocol_registry_set_automotive_mode(bool enabled);
const SubGhzProtocolRegistry* subghz_protocol_registry_active(void);

typedef struct SubGhzProtocolDecoderBinRAW SubGhzProtocolDecoderBinRAW;

#ifdef __cplusplus
}
#endif
