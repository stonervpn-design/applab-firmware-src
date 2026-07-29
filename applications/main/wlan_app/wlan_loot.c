#include "wlan_loot.h"
#include <storage/storage.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <stdio.h>
#include <string.h>

#define LOOT_ROOT "/ext/loot"

static char s_session_dir[48]; /* "/ext/loot/YYYYMMDD_HHMMSS" */
static bool s_dir_created = false;
static FuriMutex* s_mutex = NULL;

void wlan_loot_init(void) {
    if(s_mutex) return; /* already fixed for this boot */
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        s_session_dir, sizeof(s_session_dir),
        LOOT_ROOT "/%04u%02u%02u_%02u%02u%02u",
        (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
        (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    s_dir_created = false;
    s_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
}

const char* wlan_loot_session_dir(void) {
    if(!s_mutex) wlan_loot_init();
    furi_mutex_acquire(s_mutex, FuriWaitForever);
    if(!s_dir_created) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(storage, LOOT_ROOT);
        storage_common_mkdir(storage, s_session_dir);
        furi_record_close(RECORD_STORAGE);
        s_dir_created = true;
    }
    furi_mutex_release(s_mutex);
    return s_session_dir;
}

/* Copy src into dst (bounded), replacing , \r \n with spaces so a field can't
 * break the CSV row. */
static void loot_csv_field(char* dst, size_t dst_sz, const char* src) {
    size_t j = 0;
    if(src) {
        for(size_t i = 0; src[i] && j < dst_sz - 1; i++) {
            char c = src[i];
            dst[j++] = (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    dst[j] = '\0';
}

void wlan_loot_record(const char* type, const char* target, const char* detail) {
    const char* dir = wlan_loot_session_dir(); /* acquires+releases the lock */
    furi_mutex_acquire(s_mutex, FuriWaitForever);
    Storage* storage = furi_record_open(RECORD_STORAGE);

    char path[80];
    snprintf(path, sizeof(path), "%s/manifest.csv", dir);
    bool exists = storage_common_stat(storage, path, NULL) == FSE_OK;

    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(!exists) {
            const char* hdr = "timestamp,type,target,detail\n";
            storage_file_write(f, hdr, strlen(hdr));
        }
        DateTime dt;
        furi_hal_rtc_get_datetime(&dt);
        char t[40], g[64], d[80];
        loot_csv_field(t, sizeof(t), type);
        loot_csv_field(g, sizeof(g), target);
        loot_csv_field(d, sizeof(d), detail);
        char line[220];
        int n = snprintf(
            line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%s\n",
            (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
            (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second, t, g, d);
        if(n > 0) {
            if(n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            storage_file_write(f, line, (size_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    furi_mutex_release(s_mutex);
}
