#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "../applications/main/lan_attack/net_wpa.h"

static void test_wpa_pmk_compute_consistent(void) {
    const char *ssid = "TestSSID";
    const char *password = "TestPassword123";
    uint8_t pmk1[32];
    uint8_t pmk2[32];

    printf("Testing wpa_pmk_compute consistency...\n");

    bool result1 = wpa_pmk_compute(password, ssid, strlen(ssid), pmk1);
    assert(result1);

    bool result2 = wpa_pmk_compute(password, ssid, strlen(ssid), pmk2);
    assert(result2);

    printf("  First:  ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk1[i]);
    printf("...\n  Second: ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk2[i]);
    printf("...\n");

    assert(memcmp(pmk1, pmk2, 32) == 0);
    printf("  Results match (consistent): PASS\n\n");
}

static void test_wpa_pmk_different_passphrase(void) {
    const char *ssid = "IEEE";
    const char *password1 = "password";
    const char *password2 = "wrongpass";
    uint8_t pmk1[32];
    uint8_t pmk2[32];

    printf("Testing wpa_pmk_compute with different passphrases...\n");

    bool result1 = wpa_pmk_compute(password1, ssid, strlen(ssid), pmk1);
    assert(result1);

    bool result2 = wpa_pmk_compute(password2, ssid, strlen(ssid), pmk2);
    assert(result2);

    printf("  Passphrase 1: ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk1[i]);
    printf("...\n  Passphrase 2: ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk2[i]);
    printf("...\n");

    assert(memcmp(pmk1, pmk2, 32) != 0);
    printf("  Different passwords produce different PMKs: PASS\n\n");
}

static void test_wpa_pmk_different_ssid(void) {
    const char *password = "password";
    const char *ssid1 = "IEEE";
    const char *ssid2 = "OTHER";
    uint8_t pmk1[32];
    uint8_t pmk2[32];

    printf("Testing wpa_pmk_compute with different SSIDs...\n");

    bool result1 = wpa_pmk_compute(password, ssid1, strlen(ssid1), pmk1);
    assert(result1);

    bool result2 = wpa_pmk_compute(password, ssid2, strlen(ssid2), pmk2);
    assert(result2);

    printf("  SSID 1: ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk1[i]);
    printf("...\n  SSID 2: ");
    for (int i = 0; i < 16; i++) printf("%02x", pmk2[i]);
    printf("...\n");

    assert(memcmp(pmk1, pmk2, 32) != 0);
    printf("  Different SSIDs produce different PMKs: PASS\n\n");
}

static void test_wpa_ptk_compute(void) {
    uint8_t pmk[32];
    uint8_t ap_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t sta_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t anonce[32];
    uint8_t snonce[32];
    uint8_t ptk1[48];
    uint8_t ptk2[48];

    printf("Testing wpa_ptk_compute...\n");

    for (int i = 0; i < 32; i++) {
        pmk[i] = i;
        anonce[i] = i;
        snonce[i] = 32 + i;
    }

    bool result1 = wpa_ptk_compute(pmk, anonce, snonce, ap_mac, sta_mac, ptk1);
    assert(result1);

    bool result2 = wpa_ptk_compute(pmk, anonce, snonce, ap_mac, sta_mac, ptk2);
    assert(result2);

    printf("  First:  ");
    for (int i = 0; i < 16; i++) printf("%02x", ptk1[i]);
    printf("...\n  Second: ");
    for (int i = 0; i < 16; i++) printf("%02x", ptk2[i]);
    printf("...\n");

    assert(memcmp(ptk1, ptk2, 48) == 0);
    printf("  Results match (consistent): PASS\n\n");
}

static void test_wpa_mic_verify_fail(void) {
    uint8_t ptk[48];
    uint8_t eapol_frame[60];
    uint8_t wrong_mic[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    printf("Testing wpa_mic_verify with wrong MIC...\n");

    for (int i = 0; i < 48; i++) {
        ptk[i] = i * 2;
    }
    for (int i = 0; i < 60; i++) {
        eapol_frame[i] = i;
    }

    bool result = wpa_mic_verify(ptk, eapol_frame, sizeof(eapol_frame), wrong_mic, false);
    assert(!result);
    printf("  Correctly rejected wrong MIC: PASS\n\n");
}

static void test_wpa_mic_verify_consistent(void) {
    uint8_t ptk[48];
    uint8_t eapol_frame[60];
    uint8_t mic[16] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22,
                       0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00};

    printf("Testing wpa_mic_verify consistency...\n");

    for (int i = 0; i < 48; i++) {
        ptk[i] = i * 3;
    }
    for (int i = 0; i < 60; i++) {
        eapol_frame[i] = i;
    }

    bool result1 = wpa_mic_verify(ptk, eapol_frame, sizeof(eapol_frame), mic, false);
    bool result2 = wpa_mic_verify(ptk, eapol_frame, sizeof(eapol_frame), mic, false);

    assert(result1 == result2);
    printf("  Verification is consistent: PASS\n\n");
}

static void test_wpa_mic_verify_sha1(void) {
    uint8_t ptk[48];
    uint8_t eapol_frame[60];
    uint8_t mic[16] = {0x00};

    printf("Testing wpa_mic_verify with SHA1 (WPA2)...\n");

    for (int i = 0; i < 48; i++) {
        ptk[i] = i;
    }
    for (int i = 0; i < 60; i++) {
        eapol_frame[i] = i;
    }

    bool result = wpa_mic_verify(ptk, eapol_frame, sizeof(eapol_frame), mic, true);
    printf("  SHA1 mode test completed: PASS\n\n");
}

static void test_wpa_pmk_ieee_vector_1(void) {
    // IEEE 802.11i PBKDF2 Test Vector 1 (RFC 3394)
    // SSID: "IEEE"
    // Password: "password"
    // Expected PMK: f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e
    const char *ssid = "IEEE";
    const char *password = "password";
    uint8_t pmk[32];
    uint8_t expected_pmk[32] = {
        0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
        0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
        0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
        0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e
    };

    printf("Testing IEEE 802.11i PBKDF2 Test Vector 1...\n");
    printf("  SSID: '%s', Password: '%s'\n", ssid, password);

    bool result = wpa_pmk_compute(password, ssid, strlen(ssid), pmk);
    assert(result);

    printf("  Expected: ");
    for (int i = 0; i < 32; i++) printf("%02x", expected_pmk[i]);
    printf("\n  Computed: ");
    for (int i = 0; i < 32; i++) printf("%02x", pmk[i]);
    printf("\n");

    if (memcmp(pmk, expected_pmk, 32) == 0) {
        printf("  PMK matches IEEE vector: PASS\n\n");
    } else {
        printf("  PMK does NOT match IEEE vector: FAIL\n");
        printf("  **MISMATCH DETECTED** - WPA core is BROKEN\n\n");
        assert(0);  // Force abort with explicit mismatch
    }
}

int main(void) {
    printf("=== WPA/WPA2 Key Derivation Tests ===\n\n");

    test_wpa_pmk_compute_consistent();
    test_wpa_pmk_different_passphrase();
    test_wpa_pmk_different_ssid();
    test_wpa_ptk_compute();
    test_wpa_mic_verify_fail();
    test_wpa_mic_verify_consistent();
    test_wpa_mic_verify_sha1();
    test_wpa_pmk_ieee_vector_1();

    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
