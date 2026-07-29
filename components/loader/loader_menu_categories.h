#pragma once
#include <stddef.h>

// Category folders for the loader main menu. Pure (no furi/gui deps) so the
// app-id -> category mapping can be host-tested.

typedef enum {
    LoaderCategorySubGhz,
    LoaderCategory24Ghz,
    LoaderCategoryLora,
    LoaderCategoryNfc,
    LoaderCategoryRfid,
    LoaderCategoryWifi,
    LoaderCategoryBluetooth,
    LoaderCategoryInfrared,
    LoaderCategoryGpio,
    LoaderCategoryUsb,
    LoaderCategoryGames,
    LoaderCategoryTools,
    LoaderCategoryMore, // catch-all: any app id not explicitly mapped
    LoaderCategoryCount,
} LoaderCategoryId;

// Map an app id (internal `.appid` or external `.path`) to its category.
// Unknown / NULL ids fall into LoaderCategoryMore so no app is ever lost.
LoaderCategoryId loader_menu_category_for(const char* appid);

// Human-readable category name (used as the folder label + submenu header).
const char* loader_menu_category_name(LoaderCategoryId cat);
