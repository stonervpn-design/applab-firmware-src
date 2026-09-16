#include "tracker_category.h"

const char* tracker_category_label(TrackerCategory c) {
    switch(c) {
    case TrackerCatEverything: return "Everything";
    case TrackerCatApple:      return "Apple";
    case TrackerCatSamsung:    return "Samsung";
    case TrackerCatTile:       return "Tile";
    case TrackerCatMeta:       return "Meta Glasses";
    case TrackerCatDrone:      return "Drone RID";
    case TrackerCatChipolo:    return "Chipolo";
    case TrackerCatPebblebee:  return "Pebblebee";
    case TrackerCatNut:        return "Nut";
    case TrackerCatSmartwatch: return "Smartwatch";
    case TrackerCatBugSweep:   return "Bug Sweep";
    default:                   return "?";
    }
}

bool tracker_category_uses_sweep(TrackerCategory c) {
    return c == TrackerCatBugSweep;
}

bool tracker_category_matches(TrackerCategory c, TrackerKind kind) {
    switch(c) {
    case TrackerCatEverything:
        return kind != TrackerKindUnknown;
    case TrackerCatApple:
        return kind == TrackerKindAirTag || kind == TrackerKindApplePP ||
               kind == TrackerKindAppleNearby;
    case TrackerCatSamsung:  return kind == TrackerKindSmartTag;
    case TrackerCatTile:     return kind == TrackerKindTile;
    case TrackerCatMeta:     return kind == TrackerKindMetaGlasses;
    case TrackerCatDrone:    return kind == TrackerKindDroneRID;
    case TrackerCatChipolo:    return kind == TrackerKindChipolo;
    case TrackerCatPebblebee:  return kind == TrackerKindPebblebee;
    case TrackerCatNut:        return kind == TrackerKindNut;
    case TrackerCatSmartwatch: return kind == TrackerKindSmartwatch;
    case TrackerCatBugSweep: return kind == TrackerKindUnknown;
    default:                 return false;
    }
}
