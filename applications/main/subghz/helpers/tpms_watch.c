#include "tpms_watch.h"

#ifndef PUBLIC_BUILD

#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TPMS_WATCH_PATH EXT_PATH("subghz/tpms_watch.txt")
#define TPMS_WATCH_HEADER "Flipper TPMS Watch List"

static uint32_t s_watch[TPMS_WATCH_MAX];
static uint8_t s_watch_count = 0;

bool tpms_watch_contains(uint32_t id) {
    if(id == 0) return false;
    for(uint8_t i = 0; i < s_watch_count; i++) {
        if(s_watch[i] == id) return true;
    }
    return false;
}

uint8_t tpms_watch_count(void) {
    return s_watch_count;
}

static void tpms_watch_save(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_always(ff, TPMS_WATCH_PATH)) {
        flipper_format_write_header_cstr(ff, TPMS_WATCH_HEADER, 1);
        for(uint8_t i = 0; i < s_watch_count; i++) {
            uint32_t id = s_watch[i];
            flipper_format_write_hex(ff, "Id", (uint8_t*)&id, sizeof(id));
        }
    }
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

void tpms_watch_load(void) {
    s_watch_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_existing(ff, TPMS_WATCH_PATH)) {
        uint32_t id = 0;
        // Read successive "Id" entries; flipper_format advances past each.
        while(s_watch_count < TPMS_WATCH_MAX &&
              flipper_format_read_hex(ff, "Id", (uint8_t*)&id, sizeof(id))) {
            if(id != 0) s_watch[s_watch_count++] = id;
        }
    }
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

bool tpms_watch_add(uint32_t id) {
    if(id == 0) return false;
    if(tpms_watch_contains(id)) return true; // already watched
    if(s_watch_count >= TPMS_WATCH_MAX) return false;
    s_watch[s_watch_count++] = id;
    tpms_watch_save();
    return true;
}

void tpms_watch_clear(void) {
    s_watch_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, TPMS_WATCH_PATH);
    furi_record_close(RECORD_STORAGE);
}

#endif // PUBLIC_BUILD
