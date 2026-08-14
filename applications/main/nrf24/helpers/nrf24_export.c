#include "nrf24_export.h"
#include <stdio.h>

int nrf24_export_format_csv(const uint8_t* levels, uint8_t n, char* out, size_t cap) {
    size_t used = 0;
    for(uint8_t i = 0; i < n; i++) {
        char row[24];
        int r = snprintf(row, sizeof(row), "%u,%u,%u\n", i, 2400u + i, levels[i]);
        if(r < 0 || used + (size_t)r >= cap) return -1;
        for(int k = 0; k < r; k++) out[used++] = row[k];
    }
    out[used] = 0;
    return (int)used;
}

bool nrf24_settings_validate(uint32_t mode, uint32_t dwell, uint8_t* mode_out, uint16_t* dwell_out) {
    if(mode > 9) return false;                 /* 10 jammer modes: 0..9 */
    if(dwell < 100 || dwell > 4000) return false;
    *mode_out = (uint8_t)mode;
    *dwell_out = (uint16_t)dwell;
    return true;
}

#ifndef NRF24_EXPORT_HOST_TEST
#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#define NRF24_DIR  "/ext/apps_data/nrf24"
#define NRF24_CONF NRF24_DIR "/rflab.conf"
#define NRF24_FILETYPE "NRF24 RF Lab State"

bool nrf24_export_scan(const uint8_t* levels, uint8_t n) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(st, NRF24_DIR);
    static uint32_t seq = 0; seq++;
    FuriString* path = furi_string_alloc();
    furi_string_printf(path, NRF24_DIR "/scan_%03lu.csv", (unsigned long)seq);
    File* f = storage_file_alloc(st);
    bool ok = false;
    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, "channel,mhz,level\n", 18);
        char row[24];
        for(uint8_t i = 0; i < n; i++) {
            int r = snprintf(row, sizeof(row), "%u,%u,%u\n", i, 2400u + i, levels[i]);
            if(r > 0) storage_file_write(f, row, (size_t)r);
        }
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool nrf24_settings_load(uint8_t* jam_mode, uint16_t* dwell_us) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(st);
    FuriString* t = furi_string_alloc();
    bool ok = false;
    uint32_t ver = 0, mode = 0, dwell = 0;
    do {
        if(!flipper_format_file_open_existing(ff, NRF24_CONF)) break;
        if(!flipper_format_read_header(ff, t, &ver)) break;
        if(furi_string_cmp_str(t, NRF24_FILETYPE) != 0) break;
        if(!flipper_format_read_uint32(ff, "Mode", &mode, 1)) break;
        if(!flipper_format_read_uint32(ff, "Dwell", &dwell, 1)) break;
        ok = nrf24_settings_validate(mode, dwell, jam_mode, dwell_us);
    } while(0);
    if(!ok) { *jam_mode = 0; *dwell_us = 250; }
    furi_string_free(t);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool nrf24_settings_save(uint8_t jam_mode, uint16_t dwell_us) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(st, NRF24_DIR);
    FlipperFormat* ff = flipper_format_file_alloc(st);
    bool ok = false;
    uint32_t v;
    do {
        if(!flipper_format_file_open_always(ff, NRF24_CONF)) break;
        if(!flipper_format_write_header_cstr(ff, NRF24_FILETYPE, 1)) break;
        v = jam_mode; if(!flipper_format_write_uint32(ff, "Mode", &v, 1)) break;
        v = dwell_us; if(!flipper_format_write_uint32(ff, "Dwell", &v, 1)) break;
        ok = true;
    } while(0);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
#endif
