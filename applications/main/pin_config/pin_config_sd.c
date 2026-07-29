#include "pin_config_sd.h"
#include "pin_config_store.h"
#include <storage/storage.h>
#include <furi.h>

#define PIN_CFG_DIR "/ext/pin_config"
#define PIN_CFG_PATH "/ext/pin_config/pins.conf"

void pin_config_load(void) {
    pin_config_reset_defaults();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, PIN_CFG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[1024];
        uint16_t n = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        pin_config_parse(buf);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

bool pin_config_save(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, PIN_CFG_DIR);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    char buf[512];
    size_t len = pin_config_serialize(buf, sizeof(buf));
    if(storage_file_open(file, PIN_CFG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = (storage_file_write(file, buf, len) == len);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
