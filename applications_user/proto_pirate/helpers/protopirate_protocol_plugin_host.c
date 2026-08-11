#include "../protopirate_app_i.h"
#include "protopirate_txrx.h"
#include "../protocols/protocol_items.h"

#include <loader/firmware_api/firmware_api.h>
#include <stdio.h>
#include <string.h>

#define TAG "ProtoPirateProtocolPlugin"

#ifdef ENABLE_EMULATE_FEATURE
// Static TX wiring pulls in every TX-capable protocol's compiled-in SubGhzProtocol
// (each carries a real encoder behind PROTOPIRATE_WITH_ENCODER).
#include "../protocols/chrysler_v0.h"
#include "../protocols/fiat_v0.h"
#include "../protocols/fiat_v1.h"
#include "../protocols/ford_v0.h"
#include "../protocols/ford_v1.h"
#include "../protocols/ford_v2.h"
#include "../protocols/honda_static.h"
#include "../protocols/honda_v1.h"
#include "../protocols/honda_v2.h"
#include "../protocols/kia_v0.h"
#include "../protocols/kia_v1.h"
#include "../protocols/kia_v2.h"
#include "../protocols/kia_v3_v4.h"
#include "../protocols/kia_v5.h"
#include "../protocols/kia_v6.h"
#include "../protocols/kia_v7.h"
#include "../protocols/mazda_v0.h"
#include "../protocols/psa.h"
#include "../protocols/renault_v0.h"
#include "../protocols/star_line.h"
#include "../protocols/subaru.h"
#include "../protocols/vag.h"
// Rolling/crypto protocols added in the second TX pass: Porsche + Mitsubishi are
// real key-less rolling encoders; Ford V3 / Fiat V2 / Scher-Khan are replay.
#include "../protocols/mitsubishi_v0.h"
#include "../protocols/porsche_touareg.h"
#include "../protocols/ford_v3.h"
#include "../protocols/fiat_v2.h"
#include "../protocols/scher_khan.h"
#endif

// --- Static plugin wiring (ESP32 port has NO runtime FAP plugin loader) ---
// On real Flipper each RX registry ships as a separate .fal ELF loaded by the
// plugin manager. This port's plugin_manager is a stub (loads nothing), so the
// five RX registry "plugins" are compiled straight into this app instead. Each
// still exposes its stock _ep() returning a FlipperAppPluginDescriptor whose
// entry_point is the ProtoPirateProtocolPlugin; we call them directly by route.
extern const FlipperAppPluginDescriptor* protopirate_am_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_am_vag_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_f4_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_honda1_plugin_ep(void);

static const ProtoPirateProtocolPlugin*
    protopirate_static_rx_plugin(ProtoPirateProtocolRegistryRoute route) {
    const FlipperAppPluginDescriptor* descriptor = NULL;
    switch(route) {
    case ProtoPirateProtocolRegistryRouteAMVag:
        descriptor = protopirate_am_vag_plugin_ep();
        break;
    case ProtoPirateProtocolRegistryRouteFMDefault:
        descriptor = protopirate_fm_plugin_ep();
        break;
    case ProtoPirateProtocolRegistryRouteFMF4:
        descriptor = protopirate_fm_f4_plugin_ep();
        break;
    case ProtoPirateProtocolRegistryRouteFMHonda1:
        descriptor = protopirate_fm_honda1_plugin_ep();
        break;
    case ProtoPirateProtocolRegistryRouteAMDefault:
    default:
        descriptor = protopirate_am_plugin_ep();
        break;
    }
    if(!descriptor ||
       descriptor->ep_api_version != PROTOPIRATE_PROTOCOL_PLUGIN_API_VERSION) {
        return NULL;
    }
    return (const ProtoPirateProtocolPlugin*)descriptor->entry_point;
}

#ifdef ENABLE_EMULATE_FEATURE
// --- Static TX plugin wiring (mirrors protopirate_static_rx_plugin) ---
// Upstream compiles protocols/plugins/protopirate_tx_protocol_plugin.c once per
// protocol (per-instance cdefines) into a .fal loaded at TX time. This port has
// no loader, so every TX-capable protocol's compiled-in SubGhzProtocol is wrapped
// here in a 1-item registry + ProtoPirateProtocolPlugin (identical layout to the
// template) and resolved by canonical protocol name. The 22 entries match the
// tx_key set in protocol_items.c's catalog; the 5 decode-only protocols (Fiat V2,
// Ford V3, Mitsubishi V0, Porsche Touareg, Scher-Khan) have encoder.alloc == NULL
// and are intentionally absent.
#define PP_TX_STATIC_LIST(X)                                     \
    X(chrysler_protocol_v0, CHRYSLER_PROTOCOL_V0_NAME)           \
    X(fiat_protocol_v0, FIAT_PROTOCOL_V0_NAME)                   \
    X(fiat_v1_protocol, FIAT_V1_PROTOCOL_NAME)                   \
    X(ford_protocol_v0, FORD_PROTOCOL_V0_NAME)                   \
    X(ford_protocol_v1, FORD_PROTOCOL_V1_NAME)                   \
    X(ford_protocol_v2, FORD_PROTOCOL_V2_NAME)                   \
    X(honda_static_protocol, HONDA_STATIC_PROTOCOL_NAME)         \
    X(honda_v1_protocol, HONDA_V1_PROTOCOL_NAME)                 \
    X(honda_v2_protocol, HONDA_V2_PROTOCOL_NAME)                 \
    X(kia_protocol_v0, KIA_PROTOCOL_V0_NAME)                     \
    X(kia_protocol_v1, KIA_PROTOCOL_V1_NAME)                     \
    X(kia_protocol_v2, KIA_PROTOCOL_V2_NAME)                     \
    X(kia_protocol_v3_v4, KIA_PROTOCOL_V3_V4_NAME)              \
    X(kia_protocol_v5, KIA_PROTOCOL_V5_NAME)                     \
    X(kia_protocol_v6, KIA_PROTOCOL_V6_NAME)                     \
    X(kia_protocol_v7, KIA_PROTOCOL_V7_NAME)                     \
    X(mazda_v0_protocol, MAZDA_PROTOCOL_V0_NAME)                \
    X(psa_protocol, PSA_PROTOCOL_NAME)                           \
    X(renault_v0_protocol, RENAULT_PROTOCOL_V0_NAME)           \
    X(subghz_protocol_star_line, SUBGHZ_PROTOCOL_STAR_LINE_NAME) \
    X(subaru_protocol, SUBARU_PROTOCOL_NAME)                     \
    X(vag_protocol, VAG_PROTOCOL_NAME)                           \
    X(mitsubishi_v0_protocol, MITSUBISHI_PROTOCOL_NAME)         \
    X(porsche_touareg_protocol, PORSCHE_CAYENNE_PROTOCOL_NAME)  \
    X(ford_protocol_v3, FORD_PROTOCOL_V3_NAME)                   \
    X(fiat_v2_protocol, FIAT_V2_PROTOCOL_NAME)                   \
    X(subghz_protocol_scher_khan, SUBGHZ_PROTOCOL_SCHER_KHAN_NAME)

#define PP_TX_DEFINE(item, name)                                       \
    static const SubGhzProtocol* const pp_tx_items_##item[] = {&item}; \
    static const SubGhzProtocolRegistry pp_tx_registry_##item = {      \
        .items = pp_tx_items_##item,                                   \
        .size = 1U,                                                    \
    };                                                                 \
    static const ProtoPirateProtocolPlugin pp_tx_plugin_##item = {     \
        .plugin_name = name,                                           \
        .kind = ProtoPirateProtocolPluginKindTx,                      \
        .route = ProtoPirateProtocolRegistryRouteAMDefault,           \
        .protocol_name = name,                                         \
        .registry = &pp_tx_registry_##item,                           \
        .release = pp_shared_upload_release,                           \
    };
PP_TX_STATIC_LIST(PP_TX_DEFINE)
#undef PP_TX_DEFINE

static const ProtoPirateProtocolPlugin* protopirate_static_tx_plugin(const char* registry_name) {
    if(!registry_name) return NULL;
#define PP_TX_LOOKUP(item, name) \
    if(strcmp(registry_name, name) == 0) return &pp_tx_plugin_##item;
    PP_TX_STATIC_LIST(PP_TX_LOOKUP)
#undef PP_TX_LOOKUP
    return NULL;
}
#endif

static const SubGhzProtocolRegistry protopirate_empty_protocol_registry = {
    .items = NULL,
    .size = 0,
};

void protopirate_unload_protocol_plugin(ProtoPirateTxRx* txrx) {
    furi_check(txrx);

    if(txrx->environment) {
        subghz_environment_set_protocol_registry(
            txrx->environment, &protopirate_empty_protocol_registry);
    }

    txrx->protocol_registry = NULL;

    if(txrx->protocol_plugin && txrx->protocol_plugin->release) {
        txrx->protocol_plugin->release();
    }
    txrx->protocol_plugin = NULL;

    if(txrx->protocol_plugin_manager) {
        plugin_manager_free(txrx->protocol_plugin_manager);
        txrx->protocol_plugin_manager = NULL;
    }

    if(txrx->plugin_resolver) {
        composite_api_resolver_free(txrx->plugin_resolver);
        txrx->plugin_resolver = NULL;
    }
}

static bool protopirate_ensure_protocol_registry_plugin(
    ProtoPirateApp* app,
    ProtoPirateProtocolRegistryRoute route,
    const SubGhzProtocolRegistry** registry) {
    furi_check(app);
    furi_check(app->txrx);
    furi_check(registry);

    *registry = NULL;

    if(!app->txrx->environment) {
        FURI_LOG_E(TAG, "Cannot load protocol plugin without radio environment");
        return false;
    }

    if(app->txrx->protocol_plugin &&
       app->txrx->protocol_plugin->kind == ProtoPirateProtocolPluginKindRx &&
       app->txrx->protocol_plugin->registry && app->txrx->protocol_registry_route == route) {
        *registry = app->txrx->protocol_plugin->registry;
        return true;
    }

    if(app->txrx->protocol_plugin || app->txrx->protocol_plugin_manager ||
       app->txrx->plugin_resolver) {
        protopirate_unload_protocol_plugin(app->txrx);
    }

    const ProtoPirateProtocolPlugin* plugin = protopirate_static_rx_plugin(route);
    if(!plugin || !plugin->registry) {
        FURI_LOG_E(TAG, "Static protocol plugin entry point is invalid");
        return false;
    }

    if(plugin->kind != ProtoPirateProtocolPluginKindRx) {
        FURI_LOG_E(TAG, "Protocol plugin kind mismatch for RX route");
        return false;
    }

    if(plugin->route != route) {
        FURI_LOG_E(
            TAG, "Protocol plugin route mismatch (expected %d got %d)", route, plugin->route);
        return false;
    }

    // No manager/resolver in the static build — the registry is a compiled-in
    // const, so nothing to free on unload.
    app->txrx->plugin_resolver = NULL;
    app->txrx->protocol_plugin_manager = NULL;
    app->txrx->protocol_plugin = plugin;
    app->txrx->protocol_registry_route = route;
    *registry = plugin->registry;
    return true;
}

#ifdef ENABLE_EMULATE_FEATURE
static bool protopirate_ensure_tx_protocol_plugin(
    ProtoPirateApp* app,
    const char* protocol_name,
    const SubGhzProtocolRegistry** registry) {
    furi_check(app);
    furi_check(app->txrx);
    furi_check(registry);

    *registry = NULL;

    if(!app->txrx->environment) {
        FURI_LOG_E(TAG, "Cannot load TX protocol plugin without radio environment");
        return false;
    }

    const ProtoPirateProtocolCatalogEntry* catalog_entry =
        protopirate_protocol_catalog_find(protocol_name);
    const char* registry_name = protopirate_protocol_catalog_canonical_name(protocol_name);
    const char* tx_key = protopirate_protocol_catalog_tx_key(protocol_name);
    if(!registry_name || !tx_key || (catalog_entry && !tx_key)) {
        FURI_LOG_W(
            TAG, "TX disabled for %s: no tx_key in catalog", registry_name ? registry_name : "?");
        return false;
    }

    // Already bound to this TX protocol? Reuse it.
    if(app->txrx->protocol_plugin &&
       app->txrx->protocol_plugin->kind == ProtoPirateProtocolPluginKindTx &&
       app->txrx->protocol_plugin->registry && app->txrx->protocol_plugin->protocol_name &&
       strcmp(app->txrx->protocol_plugin->protocol_name, registry_name) == 0) {
        *registry = app->txrx->protocol_plugin->registry;
        return true;
    }

    if(app->txrx->protocol_plugin || app->txrx->protocol_plugin_manager ||
       app->txrx->plugin_resolver) {
        protopirate_unload_protocol_plugin(app->txrx);
    }

    // Static resolve — this port has no runtime .fal loader. The plugin is a
    // compiled-in const wrapping the protocol's compiled-in encoder.
    const ProtoPirateProtocolPlugin* plugin = protopirate_static_tx_plugin(registry_name);
    if(!plugin || plugin->kind != ProtoPirateProtocolPluginKindTx || !plugin->registry ||
       plugin->registry->size == 0U || !plugin->protocol_name ||
       strcmp(plugin->protocol_name, registry_name) != 0) {
        FURI_LOG_E(TAG, "No static TX protocol plugin for %s", registry_name);
        return false;
    }

    const SubGhzProtocol* tx_protocol = plugin->registry->items[0];
    if(!tx_protocol || !tx_protocol->encoder || !tx_protocol->encoder->alloc ||
       !tx_protocol->encoder->deserialize || !tx_protocol->encoder->yield) {
        FURI_LOG_E(TAG, "Static TX protocol %s has no usable encoder", registry_name);
        return false;
    }

    // No manager/resolver in the static build — nothing to free on unload beyond
    // the plugin's .release (pp_shared_upload_release), which unload still calls.
    app->txrx->plugin_resolver = NULL;
    app->txrx->protocol_plugin_manager = NULL;
    app->txrx->protocol_plugin = plugin;
    *registry = plugin->registry;
    return true;
}
#endif

bool protopirate_refresh_protocol_registry(ProtoPirateApp* app, bool ensure_receiver_ready) {
    furi_check(app);
    furi_check(app->txrx);

    if(!app->txrx->environment || !app->txrx->preset) {
        return true;
    }

    const char* preset_name = furi_string_get_cstr(app->txrx->preset->name);
    ProtoPirateProtocolRegistryRoute route = protopirate_get_protocol_registry_route(
        preset_name,
        app->txrx->preset->frequency,
        app->txrx->preset->data,
        app->txrx->preset->data_size,
        NULL);
    bool route_changed = !app->txrx->protocol_plugin ||
                         (app->txrx->protocol_plugin->kind != ProtoPirateProtocolPluginKindRx) ||
                         (app->txrx->protocol_registry_route != route);

    if(route_changed) {
        protopirate_rx_stack_teardown_for_registry_switch(app);
    } else if(ensure_receiver_ready && !app->txrx->receiver) {
        protopirate_rx_stack_teardown_for_registry_switch(app);
    }

    const SubGhzProtocolRegistry* registry = NULL;
    if(!protopirate_ensure_protocol_registry_plugin(app, route, &registry) || !registry) {
        FURI_LOG_E(
            TAG,
            "Failed to resolve %s protocol registry plugin",
            protopirate_get_protocol_registry_route_name(route));
        return false;
    }

    const bool registry_already_bound = (app->txrx->protocol_registry == registry);
    if(!registry_already_bound) {
        FURI_LOG_I(
            TAG,
            "Using %s protocol registry (%zu protocols)",
            protopirate_get_protocol_registry_route_name(route),
            registry->size);
        subghz_environment_set_protocol_registry(app->txrx->environment, registry);
        app->txrx->protocol_registry = registry;
    }

    if(!ensure_receiver_ready) {
        return true;
    }

    if(app->txrx->receiver) {
        return true;
    }

    app->txrx->receiver = subghz_receiver_alloc_init(app->txrx->environment);
    if(!app->txrx->receiver) {
        FURI_LOG_E(
            TAG,
            "Failed to allocate receiver for %s registry",
            protopirate_get_protocol_registry_route_name(route));
        return false;
    }

    subghz_receiver_set_filter(app->txrx->receiver, SubGhzProtocolFlag_Decodable);
    return true;
}

static bool protopirate_ensure_receiver_allocated(ProtoPirateApp* app) {
    furi_check(app);
    furi_check(app->txrx);

    if(app->txrx->receiver) {
        return true;
    }
    if(!app->txrx->environment) {
        return false;
    }

    app->txrx->receiver = subghz_receiver_alloc_init(app->txrx->environment);
    if(!app->txrx->receiver) {
        FURI_LOG_E(TAG, "Failed to allocate receiver after registry restore");
        return false;
    }

    subghz_receiver_set_filter(app->txrx->receiver, SubGhzProtocolFlag_Decodable);
    return true;
}

bool protopirate_apply_protocol_registry_for_context(
    ProtoPirateApp* app,
    const char* preset_name,
    uint32_t frequency,
    const uint8_t* preset_data,
    size_t preset_data_size,
    const char* protocol_name) {
    furi_check(app);
    furi_check(app->txrx);

    if(!app->txrx->environment) {
        return false;
    }

    if(!preset_name && app->txrx->preset && app->txrx->preset->name) {
        preset_name = furi_string_get_cstr(app->txrx->preset->name);
    }
    if(frequency == 0U && app->txrx->preset) {
        frequency = app->txrx->preset->frequency;
    }
    if((!preset_data || preset_data_size == 0U) && app->txrx->preset) {
        preset_data = app->txrx->preset->data;
        preset_data_size = app->txrx->preset->data_size;
    }

    if(protocol_name && protocol_name[0] != '\0') {
#ifdef ENABLE_EMULATE_FEATURE
        const char* registry_name = protopirate_protocol_catalog_canonical_name(protocol_name);
        if(!registry_name || !protopirate_protocol_catalog_can_tx(protocol_name)) {
            FURI_LOG_E(TAG, "No TX protocol plugin for %s", protocol_name);
            return false;
        }
        bool tx_changed = !app->txrx->protocol_plugin ||
                          (app->txrx->protocol_plugin->kind != ProtoPirateProtocolPluginKindTx) ||
                          !app->txrx->protocol_plugin->protocol_name ||
                          strcmp(app->txrx->protocol_plugin->protocol_name, registry_name) != 0;

        if(tx_changed) {
            protopirate_rx_stack_teardown_for_registry_switch(app);
        }

        const SubGhzProtocolRegistry* tx_registry = NULL;
        if(!protopirate_ensure_tx_protocol_plugin(app, registry_name, &tx_registry) ||
           !tx_registry) {
            FURI_LOG_E(TAG, "Failed to resolve TX protocol plugin for %s", protocol_name);
            return false;
        }

        if(app->txrx->protocol_registry == tx_registry) {
            return true;
        }

        FURI_LOG_I(
            TAG,
            "Switching active protocol registry to TX %s",
            registry_name ? registry_name : "?");
        subghz_environment_set_protocol_registry(app->txrx->environment, tx_registry);
        app->txrx->protocol_registry = tx_registry;
        return true;
#else
        return false;
#endif
    }

    ProtoPirateProtocolRegistryRoute route = protopirate_get_protocol_registry_route(
        preset_name, frequency, preset_data, preset_data_size, NULL);

    bool route_changed = !app->txrx->protocol_plugin ||
                         (app->txrx->protocol_plugin->kind != ProtoPirateProtocolPluginKindRx) ||
                         (app->txrx->protocol_registry_route != route);

    if(route_changed) {
        protopirate_rx_stack_teardown_for_registry_switch(app);
    }

    const SubGhzProtocolRegistry* registry = NULL;
    if(!protopirate_ensure_protocol_registry_plugin(app, route, &registry) || !registry) {
        FURI_LOG_E(
            TAG,
            "Failed to resolve %s registry plugin for preset apply",
            protopirate_get_protocol_registry_route_name(route));
        return false;
    }

    if(app->txrx->protocol_registry == registry) {
        return protopirate_ensure_receiver_allocated(app);
    }

    FURI_LOG_I(
        TAG,
        "Switching active protocol registry to %s (%zu protocols)",
        protopirate_get_protocol_registry_route_name(route),
        registry->size);
    subghz_environment_set_protocol_registry(app->txrx->environment, registry);
    app->txrx->protocol_registry = registry;
    return protopirate_ensure_receiver_allocated(app);
}
