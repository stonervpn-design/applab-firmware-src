#include "tpms_survey.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>

static const char CSV_HEADER[] =
    "uptime_ms,protocol,id,pressure_bar,temp_c,batt,freq_mhz,rssi_dbm\n";

#define TPMS_SURVEY_MAX_IDS 128

struct TpmsSurveyLog {
    Storage* storage;
    File* file;
    uint32_t count;
    uint32_t ids[TPMS_SURVEY_MAX_IDS]; // distinct sensor IDs seen (bounded set)
    uint16_t unique;
};

// Record a distinct ID; returns true if it was new (first time this session).
static bool tpms_survey_note_id(TpmsSurveyLog* log, uint32_t id) {
    for(uint16_t i = 0; i < log->unique; i++) {
        if(log->ids[i] == id) return false;
    }
    if(log->unique < TPMS_SURVEY_MAX_IDS) log->ids[log->unique++] = id;
    return true;
}

TpmsSurveyLog* tpms_survey_open(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TPMS_SURVEY_DIR);

    bool existed = (storage_common_stat(storage, TPMS_SURVEY_PATH, NULL) == FSE_OK);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, TPMS_SURVEY_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    TpmsSurveyLog* log = malloc(sizeof(TpmsSurveyLog));
    if(!log) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    log->storage = storage;
    log->file = file;
    log->count = 0;
    log->unique = 0;

    if(!existed) storage_file_write(file, CSV_HEADER, sizeof(CSV_HEADER) - 1);
    return log;
}

void tpms_survey_close(TpmsSurveyLog* log) {
    if(!log) return;
    if(log->file) {
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) furi_record_close(RECORD_STORAGE);
    free(log);
}

void tpms_survey_log(
    TpmsSurveyLog* log,
    const char* protocol,
    uint32_t id,
    float pressure,
    float temperature,
    uint8_t battery_low,
    uint32_t frequency,
    int rssi_dbm) {
    if(!log || !log->file) return;

    tpms_survey_note_id(log, id);

    char batt[8];
    if(battery_low == 0xFF)
        snprintf(batt, sizeof(batt), "?");
    else
        snprintf(batt, sizeof(batt), "%s", battery_low ? "low" : "ok");

    // Quote the protocol name (may contain a comma/space); protocol names here
    // never contain a double-quote, so a plain wrap is safe.
    char buf[160];
    int n = snprintf(
        buf,
        sizeof(buf),
        "%lu,\"%s\",0x%08lX,%.2f,%.0f,%s,%.2f,%d\n",
        (unsigned long)furi_get_tick(),
        protocol ? protocol : "?",
        (unsigned long)id,
        (double)pressure,
        (double)temperature,
        batt,
        (double)frequency / 1e6,
        rssi_dbm);
    if(n > 0) {
        if(n > (int)sizeof(buf)) n = (int)sizeof(buf);
        storage_file_write(log->file, buf, n);
        log->count++;
    }
}

uint32_t tpms_survey_count(TpmsSurveyLog* log) {
    return log ? log->count : 0;
}

uint16_t tpms_survey_unique(TpmsSurveyLog* log) {
    return log ? log->unique : 0;
}
