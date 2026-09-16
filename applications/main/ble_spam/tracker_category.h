#pragma once

#include <stdbool.h>
#include "ble_tracker_hal.h"   // TrackerKind

// A fox-hunt target category. Each maps to one or more TrackerKind values so it
// catches all variants of that type.
typedef enum {
    TrackerCatEverything = 0,  // all known kinds (not Unknown)
    TrackerCatApple,           // AirTag + AirPods (ApplePP) + iPhone/Watch (Nearby)
    TrackerCatSamsung,         // SmartTag
    TrackerCatTile,            // Tile
    TrackerCatMeta,            // Meta glasses (mfg 0x058E)
    TrackerCatDrone,           // Drone Remote ID
    TrackerCatChipolo,         // Chipolo (service UUID 0xFE33/0xFE65)
    TrackerCatPebblebee,       // Pebblebee (local name PB-/FNDR/Pebblebee)
    TrackerCatNut,             // Nut / Nutale (local name "nut")
    TrackerCatSmartwatch,      // Mi Band/Amazfit/Huawei/Fitbit/Garmin wearables
    TrackerCatBugSweep,        // Unknown devices (HAL sweep mode)
    TrackerCatCount,
} TrackerCategory;

const char* tracker_category_label(TrackerCategory c);
// True only for Bug Sweep (the HAL must capture Unknown devices).
bool tracker_category_uses_sweep(TrackerCategory c);
// Does a classified device kind belong in this category's filtered list?
bool tracker_category_matches(TrackerCategory c, TrackerKind kind);
