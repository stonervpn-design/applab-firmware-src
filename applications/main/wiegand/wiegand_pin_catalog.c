#include "wiegand_pin_catalog.h"

size_t wiegand_catalog_count(bool include_advanced) {
    return gpio_catalog_count(include_advanced);
}

const GpioCatalogEntry* wiegand_catalog_get(size_t i, bool include_advanced) {
    return gpio_catalog_get(i, include_advanced);
}
