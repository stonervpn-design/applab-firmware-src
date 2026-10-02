#include "metroflip_static_plugins.h"
#include <flipper_application/flipper_application.h>
#include <string.h>

/* Each card plugin still exposes its stock _ep() returning a FlipperAppPluginDescriptor
 * whose entry_point is the MetroflipPlugin. On the ESP32 port there is no .fal loader,
 * so the plugins are compiled in and we resolve them directly by card_type here. */

typedef const FlipperAppPluginDescriptor* (*MetroflipPluginEp)(void);

extern const FlipperAppPluginDescriptor* atr_plugin_ep(void);
extern const FlipperAppPluginDescriptor* bip_plugin_ep(void);
extern const FlipperAppPluginDescriptor* calypso_plugin_ep(void);
extern const FlipperAppPluginDescriptor* charliecard_plugin_ep(void);
extern const FlipperAppPluginDescriptor* clipper_plugin_ep(void);
extern const FlipperAppPluginDescriptor* gocard_plugin_ep(void);
extern const FlipperAppPluginDescriptor* intertic_plugin_ep(void);
extern const FlipperAppPluginDescriptor* itso_plugin_ep(void);
extern const FlipperAppPluginDescriptor* metromoney_plugin_ep(void);
extern const FlipperAppPluginDescriptor* myki_plugin_ep(void);
extern const FlipperAppPluginDescriptor* nol_plugin_ep(void);
extern const FlipperAppPluginDescriptor* opal_plugin_ep(void);
extern const FlipperAppPluginDescriptor* renfe_regular_plugin_ep(void);
extern const FlipperAppPluginDescriptor* renfe_sum10_plugin_ep(void);
extern const FlipperAppPluginDescriptor* smartrider_plugin_ep(void);
extern const FlipperAppPluginDescriptor* tmobilitat_plugin_ep(void);
extern const FlipperAppPluginDescriptor* tmoney_plugin_ep(void);
extern const FlipperAppPluginDescriptor* troika_plugin_ep(void);
extern const FlipperAppPluginDescriptor* trt_plugin_ep(void);
extern const FlipperAppPluginDescriptor* two_cities_plugin_ep(void);

typedef struct {
    const char* key; /* == app->card_type */
    MetroflipPluginEp ep;
} MetroflipStaticPlugin;

static const MetroflipStaticPlugin s_plugins[] = {
    {"atr", atr_plugin_ep},
    {"bip", bip_plugin_ep},
    {"calypso", calypso_plugin_ep},
    {"charliecard", charliecard_plugin_ep},
    {"clipper", clipper_plugin_ep},
    {"gocard", gocard_plugin_ep},
    {"intertic", intertic_plugin_ep},
    {"itso", itso_plugin_ep},
    {"metromoney", metromoney_plugin_ep},
    {"myki", myki_plugin_ep},
    {"nol", nol_plugin_ep},
    {"opal", opal_plugin_ep},
    {"renfe_regular", renfe_regular_plugin_ep},
    {"renfe_sum10", renfe_sum10_plugin_ep},
    {"smartrider", smartrider_plugin_ep},
    {"tmobilitat", tmobilitat_plugin_ep},
    {"tmoney", tmoney_plugin_ep},
    {"troika", troika_plugin_ep},
    {"trt", trt_plugin_ep},
    {"two_cities", two_cities_plugin_ep},
};

const MetroflipPlugin* metroflip_get_static_plugin(const char* card_type) {
    if(!card_type) return NULL;
    for(size_t i = 0; i < sizeof(s_plugins) / sizeof(s_plugins[0]); i++) {
        if(strcmp(s_plugins[i].key, card_type) == 0) {
            const FlipperAppPluginDescriptor* d = s_plugins[i].ep();
            if(!d) return NULL;
            return (const MetroflipPlugin*)d->entry_point;
        }
    }
    return NULL;
}
