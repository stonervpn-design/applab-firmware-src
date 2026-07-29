#include "defender_logger_fmt.h"
#include "defender_logger.h"
#include <furi.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>
#include <string.h>

/**
 * DefenderLogger opaque structure.
 */
struct DefenderLogger {
    Storage* storage;
    File* file;
    uint8_t rows_since_flush;
};

/**
 * Open the SD logger: create CSV file with header, or return NULL on failure.
 * Logging becomes a no-op if this returns NULL.
 */
DefenderLogger* defender_logger_open(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return NULL;

    /* Create apps_data/defender directory */
    storage_common_mkdir(storage, "/ext/apps_data");
    storage_common_mkdir(storage, "/ext/apps_data/defender");

    /* Build timestamped path: events-YYYYMMDD-HHMMSS.csv */
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char path[96];
    snprintf(
        path, sizeof(path),
        "/ext/apps_data/defender/events-%04u%02u%02u-%02u%02u%02u.csv",
        (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
        (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);

    /* Open file for writing */
    File* file = storage_file_alloc(storage);
    if(!file) {
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    /* Write header row */
    const char* header = "ts_ms,category,severity,offender_mac,rssi,message\n";
    if(!storage_file_write(file, header, strlen(header))) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    /* Allocate and initialize logger */
    DefenderLogger* logger = malloc(sizeof(DefenderLogger));
    if(!logger) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    logger->storage = storage;
    logger->file = file;
    logger->rows_since_flush = 0;

    return logger;
}

/**
 * Close the logger and free resources.
 */
void defender_logger_close(DefenderLogger* logger) {
    if(!logger) return;

    if(logger->file) {
        storage_file_sync(logger->file);
        storage_file_close(logger->file);
        storage_file_free(logger->file);
    }

    if(logger->storage) {
        furi_record_close(RECORD_STORAGE);
    }

    free(logger);
}

/**
 * Log an alert: format row and append to file.
 * Flushes every 8 rows.
 */
void defender_logger_log_alert(DefenderLogger* logger, const DefenderAlert* a) {
    if(!logger || !logger->file) return;

    char row[96];
    size_t n = defender_logger_format_row(row, sizeof(row), a);
    if(n == 0) return;  /* Format failed (overflow) */

    storage_file_write(logger->file, row, n);

    /* Bounded flush */
    if(++logger->rows_since_flush >= 8) {
        storage_file_sync(logger->file);
        logger->rows_since_flush = 0;
    }
}

/**
 * Phase 1 STUBS (implemented in Phase 2):
 */

bool defender_logger_pcap_begin(DefenderLogger* logger, const char* tag, uint32_t max_packets) {
    UNUSED(logger);
    UNUSED(tag);
    UNUSED(max_packets);
    /* Phase 2 */
    return false;
}

void defender_logger_pcap_write(DefenderLogger* logger, const uint8_t* frame, uint16_t len, uint32_t ts_us) {
    UNUSED(logger);
    UNUSED(frame);
    UNUSED(len);
    UNUSED(ts_us);
    /* Phase 2 */
}

void defender_logger_pcap_end(DefenderLogger* logger) {
    UNUSED(logger);
    /* Phase 2 */
}
