#include "../applications/main/defender/defender_rogue_ap.c"
#include <assert.h>
#include <stdio.h>

static DefenderBeaconRec mk(const char* ssid, uint8_t b0, uint32_t ts) {
    DefenderBeaconRec r;
    memset(&r, 0, sizeof(r));
    if(ssid) {
        strncpy(r.ssid, ssid, ROGUE_SSID_MAX - 1);
    }
    r.bssid[0] = 0xAA;
    r.bssid[5] = b0; // vary last byte -> distinct BSSID
    r.channel = 6;
    r.rssi = -50;
    r.ts_ms = ts;
    return r;
}

int main(void) {
    DefenderRogueResult res;

    // --- normal: 3 SSIDs, each one BSSID -> no alerts ---
    DefenderBeaconRec normal[3] = {mk("Home", 1, 1000), mk("Cafe", 2, 1000), mk("Office", 3, 1000)};
    defender_rogue_eval(normal, 3, 1000, 5000, &res);
    assert(!res.eviltwin_alert && !res.flood_alert);
    assert(res.distinct_ssids == 3 && res.ap_count == 3);

    // --- evil twin: same SSID from 2 different BSSIDs ---
    DefenderBeaconRec twin[3] = {
        mk("HomeNet", 1, 1000), mk("HomeNet", 2, 1000), mk("Other", 3, 1000)};
    defender_rogue_eval(twin, 3, 1000, 5000, &res);
    assert(res.eviltwin_alert);
    assert(strcmp(res.eviltwin_ssid, "HomeNet") == 0);
    assert(res.eviltwin_bssids == 2);
    assert(res.ap_count == 3);

    // same SSID from same BSSID repeatedly is NOT an evil twin
    DefenderBeaconRec same[3] = {mk("HomeNet", 1, 1000), mk("HomeNet", 1, 1010), mk("HomeNet", 1, 1020)};
    defender_rogue_eval(same, 3, 1020, 5000, &res);
    assert(!res.eviltwin_alert && res.ap_count == 1 && res.distinct_ssids == 1);

    // --- beacon flood: 15 distinct SSIDs in window ---
    DefenderBeaconRec flood[16];
    char nm[16];
    for(int i = 0; i < 16; i++) {
        snprintf(nm, sizeof(nm), "Flood%02d", i);
        flood[i] = mk(nm, (uint8_t)(10 + i), 2000);
    }
    defender_rogue_eval(flood, 16, 2000, 5000, &res);
    assert(res.flood_alert);
    assert(res.distinct_ssids == 16);

    // just under threshold: no flood
    defender_rogue_eval(flood, 10, 2000, 5000, &res);
    assert(!res.flood_alert && res.distinct_ssids == 10);

    // --- sliding window: old records excluded ---
    DefenderBeaconRec aged[2] = {mk("HomeNet", 1, 100), mk("HomeNet", 2, 9000)};
    // window 5000 ending at 9000 -> cutoff 4000, first record (ts=100) excluded
    defender_rogue_eval(aged, 2, 9000, 5000, &res);
    assert(!res.eviltwin_alert && res.ap_count == 1);

    // --- hidden SSID (empty) ignored for SSID logic but counted as AP ---
    DefenderBeaconRec hidden[2] = {mk("", 1, 1000), mk("", 2, 1000)};
    defender_rogue_eval(hidden, 2, 1000, 5000, &res);
    assert(!res.eviltwin_alert && res.distinct_ssids == 0 && res.ap_count == 2);

    printf("test_defender_rogue OK\n");
    return 0;
}
