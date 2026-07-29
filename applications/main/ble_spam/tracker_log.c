#include "tracker_log.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

static const char CSV_HEADER[] =
    "uptime_ms,category,kind,address,name,best_rssi,event\n";

struct TrackerLog {
    Storage* storage;
    File* file;
};

TrackerLog* tracker_log_open(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TRACKER_LOG_DIR);

    bool existed = (storage_common_stat(storage, TRACKER_LOG_PATH, NULL) == FSE_OK);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, TRACKER_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    TrackerLog* log = malloc(sizeof(TrackerLog));
    if(!log) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    log->storage = storage;
    log->file = file;

    if(!existed) storage_file_write(file, CSV_HEADER, sizeof(CSV_HEADER) - 1);
    return log;
}

void tracker_log_close(TrackerLog* log) {
    if(!log) return;
    if(log->file) {
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) furi_record_close(RECORD_STORAGE);
    free(log);
}

// Write the name as a CSV-escaped quoted field (doubles embedded quotes,
// replaces newlines with spaces). Same approach as ble_auto_walk_log.c.
static void write_name_quoted(File* f, const char* s) {
    storage_file_write(f, "\"", 1);
    if(s) {
        const char* run = s;
        const char* p = s;
        while(*p) {
            if(*p == '"') {
                if(p > run) storage_file_write(f, run, p - run);
                storage_file_write(f, "\"\"", 2);
                run = p + 1;
            } else if(*p == '\n' || *p == '\r') {
                if(p > run) storage_file_write(f, run, p - run);
                storage_file_write(f, " ", 1);
                run = p + 1;
            }
            p++;
        }
        if(p > run) storage_file_write(f, run, p - run);
    }
    storage_file_write(f, "\"", 1);
}

void tracker_log_alert(TrackerLog* log, TrackerCategory cat, const TrackerDevice* dev) {
    if(!log || !log->file || !dev) return;

    char buf[128];
    int n = snprintf(
        buf, sizeof(buf),
        "%lu,%s,%s,%02X:%02X:%02X:%02X:%02X:%02X,",
        (unsigned long)furi_get_tick(),
        tracker_category_label(cat),
        ble_tracker_kind_label(dev->kind),
        dev->addr[0], dev->addr[1], dev->addr[2],
        dev->addr[3], dev->addr[4], dev->addr[5]);
    if(n > 0) storage_file_write(log->file, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) - 1 : n);

    write_name_quoted(log->file, dev->name);

    n = snprintf(buf, sizeof(buf), ",%d,FOLLOW\n", dev->best_rssi);
    if(n > 0) storage_file_write(log->file, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) - 1 : n);
}
