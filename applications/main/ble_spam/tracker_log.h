#pragma once

#include "tracker_category.h"
#include "ble_tracker_hal.h"   // TrackerDevice

#define TRACKER_LOG_DIR  "/ext/ble"
#define TRACKER_LOG_PATH "/ext/ble/tracker_hunt.csv"

typedef struct TrackerLog TrackerLog;

// Opens (creates) the CSV in append mode, writing the header if new. Returns NULL
// if storage is unavailable (e.g. SD-less StickS3) — callers then no-op on log.
TrackerLog* tracker_log_open(void);
void tracker_log_close(TrackerLog* log);

// One CSV row for a new FOLLOW! alert. No-op if log==NULL.
void tracker_log_alert(TrackerLog* log, TrackerCategory cat, const TrackerDevice* dev);
