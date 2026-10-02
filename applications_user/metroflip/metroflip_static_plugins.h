#pragma once
#include "metroflip_plugins.h"

/* Static plugin registry for the ESP32 port (no FAP/FAL loader). Looks up a
 * compiled-in card plugin by card_type string, mirroring the ProtoPirate port. */
const MetroflipPlugin* metroflip_get_static_plugin(const char* card_type);
