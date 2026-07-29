/* Host test — compile with gcc, no libcheck. Pure-logic coverage of the PMKID
 * frame builders and the existing EAPOL PMKID extractor. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../applications/main/wlan_app/pmkid_frames.h"
#include "../applications/main/wlan_app/wlan_handshake_parser.h"

static const uint8_t BSSID[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
static const uint8_t SRC[6]   = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

static void test_auth_open(void) {
    uint8_t f[64];
    size_t n = pmkid_build_auth_open(f, sizeof(f), BSSID, SRC);
    assert(n == 30);
    assert(f[0] == 0xB0 && f[1] == 0x00);            /* mgmt, subtype Authentication */
    assert(memcmp(&f[4], BSSID, 6) == 0);            /* addr1 = BSSID */
    assert(memcmp(&f[10], SRC, 6) == 0);             /* addr2 = SRC */
    assert(memcmp(&f[16], BSSID, 6) == 0);           /* addr3 = BSSID */
    assert(f[24] == 0x00 && f[25] == 0x00);          /* auth algo = open */
    assert(f[26] == 0x01 && f[27] == 0x00);          /* auth seq = 1 */
    assert(f[28] == 0x00 && f[29] == 0x00);          /* status = 0 */
    printf("test_auth_open OK (%zu bytes)\n", n);
}

static void test_auth_open_cap_overflow(void) {
    uint8_t f[8];
    assert(pmkid_build_auth_open(f, sizeof(f), BSSID, SRC) == 0);
    printf("test_auth_open_cap_overflow OK\n");
}

static void test_assoc_req(void) {
    uint8_t f[128];
    const char* ssid = "TestNet";
    size_t n = pmkid_build_assoc_req(f, sizeof(f), BSSID, SRC, ssid);
    /* 24 hdr + 4 (cap+listen) + (2+7 ssid) + (2+8 rates) + (2+20 rsn) = 69 */
    assert(n == 69);
    assert(n <= 128);
    assert(f[0] == 0x00 && f[1] == 0x00);            /* mgmt, subtype Assoc Req */
    assert(memcmp(&f[4], BSSID, 6) == 0);
    assert(memcmp(&f[10], SRC, 6) == 0);
    /* capability info + listen interval */
    assert(f[24] == 0x11 && f[25] == 0x00);          /* ESS + Privacy */
    assert(f[26] == 0x0A && f[27] == 0x00);          /* listen interval */
    /* SSID tag */
    assert(f[28] == 0x00 && f[29] == 0x07);
    assert(memcmp(&f[30], "TestNet", 7) == 0);
    /* Supported rates tag right after SSID */
    assert(f[37] == 0x01 && f[38] == 0x08);
    /* RSN IE: find it after rates (offset 47) */
    size_t rsn = 47;
    assert(f[rsn] == 0x30 && f[rsn + 1] == 0x14);    /* RSN IE, len 20 */
    assert(f[rsn + 2] == 0x01 && f[rsn + 3] == 0x00);/* version 1 */
    assert(f[rsn + 4] == 0x00 && f[rsn + 5] == 0x0F && f[rsn + 6] == 0xAC && f[rsn + 7] == 0x04); /* group CCMP */
    assert(f[rsn + 8] == 0x01 && f[rsn + 9] == 0x00);/* pairwise count 1 */
    assert(f[rsn + 10] == 0x00 && f[rsn + 11] == 0x0F && f[rsn + 12] == 0xAC && f[rsn + 13] == 0x04); /* pairwise CCMP */
    assert(f[rsn + 14] == 0x01 && f[rsn + 15] == 0x00); /* akm count 1 */
    assert(f[rsn + 16] == 0x00 && f[rsn + 17] == 0x0F && f[rsn + 18] == 0xAC && f[rsn + 19] == 0x02); /* akm PSK */
    assert(f[rsn + 20] == 0x00 && f[rsn + 21] == 0x00); /* RSN caps */
    printf("test_assoc_req OK (%zu bytes)\n", n);
}

static void test_assoc_req_cap_overflow(void) {
    uint8_t f[32];
    assert(pmkid_build_assoc_req(f, sizeof(f), BSSID, SRC, "TestNet") == 0);
    printf("test_assoc_req_cap_overflow OK\n");
}

static void test_extract_pmkid(void) {
    /* Build a minimal EAPOL body: key-data length at [97..98]=22, KDE at [99]. */
    uint8_t eapol[121];
    memset(eapol, 0, sizeof(eapol));
    eapol[97] = 0x00;
    eapol[98] = 0x16;                                /* key data len = 22 */
    uint8_t kde[] = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04};
    memcpy(&eapol[99], kde, sizeof(kde));
    for(int i = 0; i < 16; i++) eapol[105 + i] = (uint8_t)(0x40 + i); /* non-zero PMKID */

    uint8_t out[16];
    assert(wlan_hs_extract_pmkid(eapol, (int)sizeof(eapol), out) == true);
    for(int i = 0; i < 16; i++) assert(out[i] == (uint8_t)(0x40 + i));
    printf("test_extract_pmkid OK\n");
}

static void test_extract_pmkid_zeroed_rejected(void) {
    uint8_t eapol[121];
    memset(eapol, 0, sizeof(eapol));
    eapol[98] = 0x16;
    uint8_t kde[] = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04};
    memcpy(&eapol[99], kde, sizeof(kde));            /* PMKID left all-zero */
    uint8_t out[16];
    assert(wlan_hs_extract_pmkid(eapol, (int)sizeof(eapol), out) == false);
    printf("test_extract_pmkid_zeroed_rejected OK\n");
}

int main(void) {
    test_auth_open();
    test_auth_open_cap_overflow();
    test_assoc_req();
    test_assoc_req_cap_overflow();
    test_extract_pmkid();
    test_extract_pmkid_zeroed_rejected();
    printf("ALL PMKID FRAME TESTS PASSED\n");
    return 0;
}
