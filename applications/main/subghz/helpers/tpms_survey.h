#pragma once

#include <stdint.h>

// TPMS Survey logger: append every decoded TPMS sensor to a CSV on the SD card so
// a parking-lot / roadside sweep can be reviewed or correlated later (one physical
// vehicle carries 4 sensors, so grouping by time+RSSI reconstructs cars). Mirrors
// ble_spam/tracker_log.c. No-op (NULL) when storage is unavailable.

#define TPMS_SURVEY_DIR  "/ext/tpms"
#define TPMS_SURVEY_PATH "/ext/tpms/survey.csv"

typedef struct TpmsSurveyLog TpmsSurveyLog;

// Open (create) the survey CSV in append mode, writing the header if new.
// Returns NULL if storage is unavailable (e.g. SD-less board) — callers no-op.
TpmsSurveyLog* tpms_survey_open(void);
void tpms_survey_close(TpmsSurveyLog* log);

// Append one row for a decoded sensor. protocol may be NULL. pressure is bar,
// temperature is degC, battery_low is 0/1 (or 0xFF = unknown). No-op if log==NULL.
void tpms_survey_log(
    TpmsSurveyLog* log,
    const char* protocol,
    uint32_t id,
    float pressure,
    float temperature,
    uint8_t battery_low,
    uint32_t frequency,
    int rssi_dbm);

// Rows written this session (for the on-screen "Survey: N logged" status).
uint32_t tpms_survey_count(TpmsSurveyLog* log);

// Distinct sensor IDs seen this session (bounded set). A vehicle carries ~4
// sensors, so vehicles ~= unique/4 (rough estimate shown on screen).
uint16_t tpms_survey_unique(TpmsSurveyLog* log);
