#include "can_commander_app_i.h"
#include <storage/storage.h>
#include <string.h>

void can_presets_load(CanCommanderApp* app) {
    app->preset_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, CAN_PRESET_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[2048];
        uint16_t n = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        char* p = buf;
        while(*p && app->preset_count < CAN_PRESET_MAX) {
            char* nl = strchr(p, '\n');
            if(nl) *nl = '\0';
            char* cr = strchr(p, '\r');
            if(cr) *cr = '\0';
            if(*p) {
                char name[CAN_PRESET_NAME_LEN];
                CanFrame f;
                if(can_frame_preset_decode(p, name, sizeof(name), &f)) {
                    strncpy(
                        app->preset_names[app->preset_count], name, CAN_PRESET_NAME_LEN - 1);
                    app->preset_names[app->preset_count][CAN_PRESET_NAME_LEN - 1] = '\0';
                    app->presets[app->preset_count] = f;
                    app->preset_count++;
                }
            }
            if(!nl) break;
            p = nl + 1;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

bool can_presets_append(CanCommanderApp* app, const char* name, const CanFrame* f) {
    if(app->preset_count >= CAN_PRESET_MAX) return false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, CAN_PRESET_DIR); // no-op if it exists
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, CAN_PRESET_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[96];
        size_t len = can_frame_preset_encode(name, f, line, sizeof(line));
        if(len > 0 && len + 1 < sizeof(line)) {
            line[len++] = '\n';
            ok = (storage_file_write(file, line, len) == len);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(ok) can_presets_load(app);
    return ok;
}
