// protocols/plugins/protopirate_tx_static_plugins.c
//
// ESP32 STATIC PORT: compiled-in TX (emulate) protocol plugins.
//
// Upstream ProtoPirate builds one tx_<key>.fal per TX-capable protocol from a
// SINGLE template (protopirate_tx_protocol_plugin.c), macro-compiled per
// protocol so every .fal exports the same entry-point symbol
// (protopirate_tx_protocol_plugin_ep). That template cannot be compiled 23x
// into one static image, so this file defines all of them once, each with a
// unique protopirate_tx_<key>_ep() symbol. The static loader
// (helpers/protopirate_plugins.c) dispatches "tx_<key>.fal" paths here.
//
// Each plugin is a single-protocol registry (kind = Tx, protocol_name = the
// catalog canonical name) exactly matching what the stock .fal exposed. This
// file is only meaningful when emulate is enabled (encoders are compiled via
// PROTOPIRATE_WITH_ENCODER); it compiles to nothing otherwise.

#include "../protopirate_protocol_plugins.h"
#include "../protocols_common.h"
#include <lib/flipper_application/flipper_application.h>

#ifdef ENABLE_EMULATE_FEATURE

#include "../chrysler_v0.h"
#include "../fiat_v0.h"
#include "../fiat_v1.h"
#include "../ford_v0.h"
#include "../ford_v1.h"
#include "../ford_v2.h"
#include "../honda_static.h"
#include "../honda_v1.h"
#include "../honda_v2.h"
#include "../kia_v0.h"
#include "../kia_v1.h"
#include "../kia_v2.h"
#include "../kia_v3_v4.h"
#include "../kia_v5.h"
#include "../kia_v6.h"
#include "../kia_v7.h"
#include "../mazda_v0.h"
#include "../psa.h"
#include "../renault_v0.h"
#include "../renault_v1.h"
#include "../star_line.h"
#include "../subaru.h"
#include "../vag.h"

// Mirrors protopirate_tx_protocol_plugin.c, but with a unique symbol set per key.
#define PP_TX_PLUGIN(key, name_macro, item)                                \
    static const SubGhzProtocol* const pp_tx_items_##key[] = {&item};      \
    static const SubGhzProtocolRegistry pp_tx_reg_##key = {                \
        .items = pp_tx_items_##key,                                        \
        .size = 1U,                                                        \
    };                                                                     \
    static const ProtoPirateProtocolPlugin pp_tx_plugin_##key = {          \
        .plugin_name = name_macro,                                         \
        .kind = ProtoPirateProtocolPluginKindTx,                           \
        .route = ProtoPirateProtocolRegistryRouteAMDefault,                \
        .protocol_name = name_macro,                                       \
        .registry = &pp_tx_reg_##key,                                      \
        .release = pp_shared_upload_release,                               \
    };                                                                     \
    static const FlipperAppPluginDescriptor pp_tx_desc_##key = {           \
        .appid = PROTOPIRATE_PROTOCOL_PLUGIN_APP_ID,                       \
        .ep_api_version = PROTOPIRATE_PROTOCOL_PLUGIN_API_VERSION,         \
        .entry_point = &pp_tx_plugin_##key,                                \
    };                                                                     \
    const FlipperAppPluginDescriptor* protopirate_tx_##key##_ep(void) {    \
        return &pp_tx_desc_##key;                                          \
    }

PP_TX_PLUGIN(chrysler_v0, CHRYSLER_PROTOCOL_V0_NAME, chrysler_protocol_v0)
PP_TX_PLUGIN(fiat_v0, FIAT_PROTOCOL_V0_NAME, fiat_protocol_v0)
PP_TX_PLUGIN(fiat_v1, FIAT_V1_PROTOCOL_NAME, fiat_v1_protocol)
PP_TX_PLUGIN(ford_v0, FORD_PROTOCOL_V0_NAME, ford_protocol_v0)
PP_TX_PLUGIN(ford_v1, FORD_PROTOCOL_V1_NAME, ford_protocol_v1)
PP_TX_PLUGIN(ford_v2, FORD_PROTOCOL_V2_NAME, ford_protocol_v2)
PP_TX_PLUGIN(honda_static, HONDA_STATIC_PROTOCOL_NAME, honda_static_protocol)
PP_TX_PLUGIN(honda_v1, HONDA_V1_PROTOCOL_NAME, honda_v1_protocol)
PP_TX_PLUGIN(kia_v0, KIA_PROTOCOL_V0_NAME, kia_protocol_v0)
PP_TX_PLUGIN(kia_v1, KIA_PROTOCOL_V1_NAME, kia_protocol_v1)
PP_TX_PLUGIN(kia_v2, KIA_PROTOCOL_V2_NAME, kia_protocol_v2)
PP_TX_PLUGIN(kia_v3_v4, KIA_PROTOCOL_V3_V4_NAME, kia_protocol_v3_v4)
PP_TX_PLUGIN(kia_v5, KIA_PROTOCOL_V5_NAME, kia_protocol_v5)
PP_TX_PLUGIN(kia_v6, KIA_PROTOCOL_V6_NAME, kia_protocol_v6)
PP_TX_PLUGIN(kia_v7, KIA_PROTOCOL_V7_NAME, kia_protocol_v7)
PP_TX_PLUGIN(honda_v2, HONDA_V2_PROTOCOL_NAME, honda_v2_protocol)
PP_TX_PLUGIN(mazda_v0, MAZDA_PROTOCOL_V0_NAME, mazda_v0_protocol)
PP_TX_PLUGIN(psa, PSA_PROTOCOL_NAME, psa_protocol)
PP_TX_PLUGIN(renault_v0, RENAULT_PROTOCOL_V0_NAME, renault_v0_protocol)
PP_TX_PLUGIN(renault_v1, RENAULT_PROTOCOL_V1_NAME, renault_v1_protocol)
PP_TX_PLUGIN(star_line, SUBGHZ_PROTOCOL_STAR_LINE_NAME, subghz_protocol_star_line)
PP_TX_PLUGIN(subaru, SUBARU_PROTOCOL_NAME, subaru_protocol)
PP_TX_PLUGIN(vag, VAG_PROTOCOL_NAME, vag_protocol)

#undef PP_TX_PLUGIN

#endif // ENABLE_EMULATE_FEATURE
