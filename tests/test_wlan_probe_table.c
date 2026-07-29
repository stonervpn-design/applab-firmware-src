#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include "../applications/main/wlan_app/wlan_probe_table.c"
#include <assert.h>
#include <stdio.h>

static void mkmac(uint8_t m[6], uint8_t last) {
    m[0]=0x02; m[1]=0; m[2]=0; m[3]=0; m[4]=0; m[5]=last;
}

int main(void) {
    WlanProbeTable t;
    wlan_probe_table_reset(&t);
    assert(wlan_probe_table_client_count(&t) == 0);

    uint8_t a[6]; mkmac(a, 1);
    wlan_probe_table_add(&t, a, "Net1");
    wlan_probe_table_add(&t, a, "Net2");
    wlan_probe_table_add(&t, a, "Net1");        // dup ssid for same client
    assert(wlan_probe_table_client_count(&t) == 1);

    // one client, two distinct SSIDs
    assert(wlan_probe_table_ssid_count(&t) == 2);

    // fill past capacity → LRU eviction, count capped
    for (int i = 2; i < WLAN_PT_CAP + 10; i++) {
        uint8_t m[6]; mkmac(m, (uint8_t)i);
        wlan_probe_table_add(&t, m, "X");
    }
    assert(wlan_probe_table_client_count(&t) == WLAN_PT_CAP);

    // ssid set is bounded at 32 and never overflows
    assert(wlan_probe_table_ssid_count(&t) <= WLAN_SSID_SET_CAP);

    printf("test_wlan_probe_table OK\n");
    return 0;
}
