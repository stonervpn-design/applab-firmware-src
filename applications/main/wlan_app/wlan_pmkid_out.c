#include "wlan_pmkid_out.h"
#include "wlan_loot.h"
#include <storage/storage.h>
#include <stdio.h>

void wlan_pmkid_save(
    Storage* storage,
    const uint8_t pmkid[16],
    const uint8_t ap[6],
    const uint8_t sta[6],
    const char* ssid) {
    if(!storage) return;
    const char* dir = wlan_loot_session_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/pmkid.22000", dir);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[160];
        int n = snprintf(line, sizeof(line), "WPA*01*");
        for(int i = 0; i < 16; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", pmkid[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", ap[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", sta[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(const char* p = ssid; *p && n < (int)sizeof(line) - 8; p++)
            n += snprintf(line + n, sizeof(line) - n, "%02x", (uint8_t)*p);
        n += snprintf(line + n, sizeof(line) - n, "***\n");
        storage_file_write(f, line, n);
        storage_file_close(f);
        wlan_loot_record("pmkid", ssid, "pmkid.22000");
    }
    storage_file_free(f);
}
