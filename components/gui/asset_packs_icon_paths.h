#pragma once
// Generated (T-Embed port): icon -> asset-pack path table for asset_packs.c.
// The base's asset pipeline doesn't emit ICON_PATHS, so we derive it from
// assets_icons.h: path = icon symbol minus its I_/A_ prefix.
#include <gui/icon.h>
#include <stddef.h>

typedef struct {
    const Icon* icon;
    const char* path;
} IconPath;

extern const IconPath ICON_PATHS[];
extern const size_t ICON_PATHS_COUNT;
