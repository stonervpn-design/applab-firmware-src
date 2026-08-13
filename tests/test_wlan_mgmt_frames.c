#include "wlan_mgmt_frames.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t out[256];
    uint8_t bssid[6] = {1,2,3,4,5,6}, sta[6] = {7,8,9,10,11,12}, src[6]={0xAA,0,0,0,0,1};
    uint16_t n = wlan_build_deauth(out, sizeof(out), bssid, sta, 7);
    assert(n == 26); assert(out[0] == 0xC0);
    assert(memcmp(out+4, sta, 6) == 0); assert(memcmp(out+10, bssid, 6) == 0);
    assert(memcmp(out+16, bssid, 6) == 0); assert(out[24]==7 && out[25]==0);
    assert(wlan_build_deauth(out, 10, bssid, sta, 7) == 0);
    n = wlan_build_probe_req(out, sizeof(out), src, "Test");
    assert(n >= 24+2+4); assert(out[0] == 0x40); assert(out[24] == 0x00 && out[25] == 4);
    assert(memcmp(out+26, "Test", 4) == 0);
    n = wlan_build_beacon(out, sizeof(out), bssid, "AP", 6);
    assert(n > 36); assert(out[0] == 0x80);
    uint8_t bc[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}; assert(memcmp(out+4, bc, 6)==0);
    /* header 24 + timestamp 8 (24-31) + interval 2 (32-33) + capability 2 (34-35) */
    assert(out[34]==0x11 && out[35]==0x00); /* capability info: ESS+Privacy (LE) */
    /* cap-too-small returns 0 for probe + beacon too (not just deauth) */
    assert(wlan_build_probe_req(out, 10, src, "Test") == 0);
    assert(wlan_build_beacon(out, 10, bssid, "AP", 6) == 0);
    /* oversized SSID (>32) is clamped, never overflows */
    char big[64]; memset(big, 'X', sizeof(big)); big[63] = 0;
    n = wlan_build_beacon(out, sizeof(out), bssid, big, 1);
    assert(n == 47 + 32); /* SSID clamped to 32 */
    printf("test_wlan_mgmt_frames: ALL PASS\n");
    return 0;
}
