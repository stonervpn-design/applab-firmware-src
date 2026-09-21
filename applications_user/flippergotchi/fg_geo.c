#include "fg_geo.h"
#include <string.h>
#include <stdio.h>
// The MENUEXTERNAL app component doesn't inherit main's include dirs and "gnss"
// isn't a valid fam `requires` token, so reach the header by relative path. The
// gnss_* symbols still resolve at the final firmware link (main REQUIRES gnss).
#include "../../components/gnss/gnss.h"

int fg_geo_format_row(
    char* out,
    size_t cap,
    const uint8_t bssid[6],
    const char* ssid,
    uint8_t channel,
    int8_t rssi,
    double lat,
    double lon) {
    return snprintf(
        out, cap, "%02X:%02X:%02X:%02X:%02X:%02X,%s,[WPA2],,%u,%d,%.6f,%.6f", bssid[0], bssid[1],
        bssid[2], bssid[3], bssid[4], bssid[5], ssid ? ssid : "", channel, rssi, lat, lon);
}

void fg_geo_init(void) {
    if(!gnss_is_running()) gnss_start();
}

bool fg_geo_has_fix(void) {
    GnssFix fix;
    return gnss_is_running() && gnss_get_fix(&fix) && fix.valid;
}

bool fg_geo_tag(
    Storage* s,
    const uint8_t bssid[6],
    const char* ssid,
    uint8_t channel,
    int8_t rssi) {
    GnssFix fix;
    if(!gnss_is_running() || !gnss_get_fix(&fix) || !fix.valid) return false;
    storage_common_mkdir(s, "/ext/wifi");
    File* f = storage_file_alloc(s);
    bool ok = false;
    if(storage_file_open(f, FG_WARDRIVE_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char row[128];
        int n = fg_geo_format_row(row, sizeof(row) - 1, bssid, ssid, channel, rssi, fix.lat, fix.lon);
        if(n > 0) {
            row[n++] = '\n';
            ok = (storage_file_write(f, row, n) == (uint32_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    return ok;
}
