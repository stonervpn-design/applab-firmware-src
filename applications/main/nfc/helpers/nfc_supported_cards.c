#include "nfc_supported_cards.h"

#include "../plugins/supported_cards/nfc_supported_card_plugin.h"
#include "nfc_supported_cards_static.h"

#include <flipper_application/flipper_application.h>
#include <flipper_application/plugins/plugin_manager.h>
#include <loader/firmware_api/firmware_api.h>

#include <furi.h>
#include <path.h>
#include <m-array.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#define TAG "NfcSupportedCards"

#define NFC_SUPPORTED_CARDS_PLUGINS_PATH  APP_DATA_PATH("plugins")
#define NFC_SUPPORTED_CARDS_PLUGIN_SUFFIX "_parser.fal"

typedef enum {
    NfcSupportedCardsPluginFeatureHasVerify = (1U << 0),
    NfcSupportedCardsPluginFeatureHasRead = (1U << 1),
    NfcSupportedCardsPluginFeatureHasParse = (1U << 2),
} NfcSupportedCardsPluginFeature;

typedef struct {
    FuriString* name;
    NfcProtocol protocol;
    NfcSupportedCardsPluginFeature feature;
} NfcSupportedCardsPluginCache;

ARRAY_DEF(NfcSupportedCardsPluginCache, NfcSupportedCardsPluginCache, M_POD_OPLIST); //-V658

typedef enum {
    NfcSupportedCardsLoadStateIdle,
    NfcSupportedCardsLoadStateInProgress,
    NfcSupportedCardsLoadStateSuccess,
    NfcSupportedCardsLoadStateFail,
} NfcSupportedCardsLoadState;

typedef struct {
    Storage* storage;
    File* directory;
    char file_name[256];
    FlipperApplication* app;
} NfcSupportedCardsLoadContext;

struct NfcSupportedCards {
    CompositeApiResolver* api_resolver;
    NfcSupportedCardsPluginCache_t plugins_cache_arr;
    NfcSupportedCardsLoadState load_state;
    NfcSupportedCardsLoadContext* load_context;
};

NfcSupportedCards* nfc_supported_cards_alloc(CompositeApiResolver* api_resolver) {
    NfcSupportedCards* instance = malloc(sizeof(NfcSupportedCards));
    instance->api_resolver = api_resolver;

    NfcSupportedCardsPluginCache_init(instance->plugins_cache_arr);

    return instance;
}

void nfc_supported_cards_free(NfcSupportedCards* instance) {
    furi_assert(instance);

    NfcSupportedCardsPluginCache_it_t iter;
    for(NfcSupportedCardsPluginCache_it(iter, instance->plugins_cache_arr);
        !NfcSupportedCardsPluginCache_end_p(iter);
        NfcSupportedCardsPluginCache_next(iter)) {
        NfcSupportedCardsPluginCache* plugin_cache = NfcSupportedCardsPluginCache_ref(iter);
        furi_string_free(plugin_cache->name);
    }
    NfcSupportedCardsPluginCache_clear(instance->plugins_cache_arr);

    free(instance);
}

static NfcSupportedCardsLoadContext* nfc_supported_cards_load_context_alloc(void) {
    NfcSupportedCardsLoadContext* instance = malloc(sizeof(NfcSupportedCardsLoadContext));
    instance->app = NULL; /* zeroed before the first get_plugin()/free() reads it */

    instance->storage = furi_record_open(RECORD_STORAGE);
    instance->directory = storage_file_alloc(instance->storage);

    if(!storage_dir_open(instance->directory, NFC_SUPPORTED_CARDS_PLUGINS_PATH)) {
        FURI_LOG_D(TAG, "Failed to open directory: %s", NFC_SUPPORTED_CARDS_PLUGINS_PATH);
    }

    return instance;
}

static void nfc_supported_cards_load_context_free(NfcSupportedCardsLoadContext* instance) {
    if(instance->app) {
        flipper_application_free(instance->app);
    }

    storage_dir_close(instance->directory);
    storage_file_free(instance->directory);

    furi_record_close(RECORD_STORAGE);
    free(instance);
}

static const NfcSupportedCardsPlugin* nfc_supported_cards_get_plugin(
    NfcSupportedCardsLoadContext* instance,
    const char* name,
    const ElfApiInterface* api_interface) {
    UNUSED(instance);
    UNUSED(api_interface);
    furi_assert(name);
    /* Static registry: parsers are compiled into the firmware and resolved by
     * calling their <name>_plugin_ep() directly (see nfc_supported_cards_static.c).
     * No runtime .fal ELF loader, so this works on no-PSRAM boards, where the loader
     * is skipped entirely (its code maps into RAM that only executes with PSRAM). */
    return nfc_supported_cards_static_get_by_name(name);
}

void nfc_supported_cards_load_cache(NfcSupportedCards* instance) {
    furi_assert(instance);

    do {
        if((instance->load_state == NfcSupportedCardsLoadStateSuccess) ||
           (instance->load_state == NfcSupportedCardsLoadStateFail))
            break;

        /* Static registry: the parsers are compiled into the firmware, so we just
         * enumerate them (no runtime .fal ELF loader). This works on every board,
         * including no-PSRAM ones (Cardputer-ADV) where the loader was skipped
         * entirely — previously leaving those boards with ZERO card parsers. */
        const size_t count = nfc_supported_cards_static_count();
        for(size_t i = 0; i < count; i++) {
            const char* name = NULL;
            const NfcSupportedCardsPlugin* plugin =
                nfc_supported_cards_static_get_by_index(i, &name);
            if(plugin == NULL) {
                /* Descriptor mismatch / null entry point — the parser won't work.
                 * Surface it loudly instead of silently dropping coverage. */
                FURI_LOG_E(TAG, "card parser '%s' (#%zu) failed to resolve", name ? name : "?", i);
                continue;
            }

            NfcSupportedCardsPluginCache plugin_cache = {}; //-V779
            plugin_cache.name = furi_string_alloc_set(name);
            plugin_cache.protocol = plugin->protocol;
            if(plugin->verify) {
                plugin_cache.feature |= NfcSupportedCardsPluginFeatureHasVerify;
            }
            if(plugin->read) {
                plugin_cache.feature |= NfcSupportedCardsPluginFeatureHasRead;
            }
            if(plugin->parse) {
                plugin_cache.feature |= NfcSupportedCardsPluginFeatureHasParse;
            }
            NfcSupportedCardsPluginCache_push_back(instance->plugins_cache_arr, plugin_cache);
        }

        size_t plugins_loaded = NfcSupportedCardsPluginCache_size(instance->plugins_cache_arr);
        if(plugins_loaded == 0) {
            FURI_LOG_D(TAG, "Plugins not found");
            instance->load_state = NfcSupportedCardsLoadStateFail;
        } else {
            FURI_LOG_I(TAG, "Loaded %zu static card parsers", plugins_loaded);
            instance->load_state = NfcSupportedCardsLoadStateSuccess;
        }

    } while(false);
}

bool nfc_supported_cards_read(NfcSupportedCards* instance, NfcDevice* device, Nfc* nfc) {
    furi_assert(instance);
    furi_assert(device);
    furi_assert(nfc);

    bool card_read = false;
    NfcProtocol protocol = nfc_device_get_protocol(device);

    do {
        if(instance->load_state != NfcSupportedCardsLoadStateSuccess) break;

        instance->load_context = nfc_supported_cards_load_context_alloc();

        NfcSupportedCardsPluginCache_it_t iter;
        for(NfcSupportedCardsPluginCache_it(iter, instance->plugins_cache_arr);
            !NfcSupportedCardsPluginCache_end_p(iter);
            NfcSupportedCardsPluginCache_next(iter)) {
            NfcSupportedCardsPluginCache* plugin_cache = NfcSupportedCardsPluginCache_ref(iter);
            if(plugin_cache->protocol != protocol) continue;
            if((plugin_cache->feature & NfcSupportedCardsPluginFeatureHasRead) == 0) continue;

            const ElfApiInterface* api_interface =
                composite_api_resolver_get(instance->api_resolver);
            const NfcSupportedCardsPlugin* plugin = nfc_supported_cards_get_plugin(
                instance->load_context, furi_string_get_cstr(plugin_cache->name), api_interface);
            if(plugin == NULL) continue;

            if(plugin->verify) {
                if(!plugin->verify(nfc)) continue;
            }

            if(plugin->read) {
                if(plugin->read(nfc, device)) {
                    card_read = true;
                    break;
                }
            }
        }

        nfc_supported_cards_load_context_free(instance->load_context);
    } while(false);

    return card_read;
}

bool nfc_supported_cards_parse(
    NfcSupportedCards* instance,
    NfcDevice* device,
    FuriString* parsed_data) {
    furi_assert(instance);
    furi_assert(device);
    furi_assert(parsed_data);

    bool card_parsed = false;
    NfcProtocol protocol = nfc_device_get_protocol(device);

    do {
        if(instance->load_state != NfcSupportedCardsLoadStateSuccess) break;

        instance->load_context = nfc_supported_cards_load_context_alloc();

        NfcSupportedCardsPluginCache_it_t iter;
        for(NfcSupportedCardsPluginCache_it(iter, instance->plugins_cache_arr);
            !NfcSupportedCardsPluginCache_end_p(iter);
            NfcSupportedCardsPluginCache_next(iter)) {
            NfcSupportedCardsPluginCache* plugin_cache = NfcSupportedCardsPluginCache_ref(iter);
            if(plugin_cache->protocol != protocol) continue;
            if((plugin_cache->feature & NfcSupportedCardsPluginFeatureHasParse) == 0) continue;

            const ElfApiInterface* api_interface =
                composite_api_resolver_get(instance->api_resolver);
            const NfcSupportedCardsPlugin* plugin = nfc_supported_cards_get_plugin(
                instance->load_context, furi_string_get_cstr(plugin_cache->name), api_interface);
            if(plugin == NULL) continue;

            if(plugin->parse) {
                if(plugin->parse(device, parsed_data)) {
                    card_parsed = true;
                    break;
                }
            }
        }

        nfc_supported_cards_load_context_free(instance->load_context);
    } while(false);

    return card_parsed;
}
