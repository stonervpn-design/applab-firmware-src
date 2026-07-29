#include "net_csv.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>

#define TAG "NetCsv"

// Capture directory tree ensured on open. Kept in sync with the paths callers
// are expected to pass to net_csv_open().
#define NET_CSV_DIR_ROOT     "/ext/lan_attack"
#define NET_CSV_DIR_CAPTURES "/ext/lan_attack/captures"

/**
 * Opaque CSV log handle. Holds the storage record and open file. The record is
 * opened per-log and released in net_csv_close().
 */
struct NetCsvLog {
    Storage* storage;
    File* file;
};

NetCsvLog* net_csv_open(const char* path) {
    if(!path) {
        FURI_LOG_W(TAG, "open: NULL path");
        return NULL;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) {
        FURI_LOG_W(TAG, "open: no storage record");
        return NULL;
    }

    // Ensure the capture directory tree exists (parents first). mkdir is a
    // no-op / benign error when the directory already exists, so we don't gate
    // on its result.
    storage_common_mkdir(storage, NET_CSV_DIR_ROOT);
    storage_common_mkdir(storage, NET_CSV_DIR_CAPTURES);

    File* file = storage_file_alloc(storage);
    if(!file) {
        FURI_LOG_W(TAG, "open: file alloc failed");
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_W(TAG, "open: cannot open '%s' for append", path);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    NetCsvLog* log = malloc(sizeof(NetCsvLog));
    if(!log) {
        FURI_LOG_W(TAG, "open: log alloc failed");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    log->storage = storage;
    log->file = file;
    return log;
}

void net_csv_write_line(NetCsvLog* log, const char* csv_line) {
    if(!log || !log->file || !csv_line) return;

    size_t len = strlen(csv_line);
    if(len > 0) {
        if(storage_file_write(log->file, csv_line, len) != len) {
            FURI_LOG_W(TAG, "write: short write on line body");
            return;
        }
    }

    if(storage_file_write(log->file, "\n", 1) != 1) {
        FURI_LOG_W(TAG, "write: short write on newline");
        return;
    }

    // Line-buffered: flush so captures survive a power loss.
    storage_file_sync(log->file);
}

void net_csv_close(NetCsvLog* log) {
    if(!log) return;

    if(log->file) {
        storage_file_sync(log->file);
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) {
        furi_record_close(RECORD_STORAGE);
    }
    free(log);
}

void net_csv_write_ntlmv2_hashcat(NetCsvLog* log, const NtlmV2Parsed* parsed) {
    if(!log || !parsed) return;

    // Reuse the existing hashcat -m 5500 formatter; do not reimplement it.
    // Buffer sized for user(256)::domain(256):challenge(16):response(32) plus
    // separators and terminator, with generous headroom.
    char line[640];
    size_t written = net_ntlm_hashcat_5500(parsed, line, sizeof(line));
    if(written == 0) {
        FURI_LOG_W(TAG, "ntlmv2: hashcat format failed");
        return;
    }

    net_csv_write_line(log, line);
}
