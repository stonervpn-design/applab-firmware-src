#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============ WPA/WPA2 PMK/PTK/MIC (pure) ============ */

/* Compute PMK (Pairwise Master Key) from PSK (password) + SSID.
 * Uses PBKDF2-SHA1 with 4096 iterations.
 * Returns true on success, false on error.
 */
bool wpa_pmk_compute(const char* password, const char* ssid, size_t ssid_len,
                      uint8_t out_pmk[32]);

/* Compute PTK (Pairwise Transient Key) from PMK + handshake nonces + MACs.
 * PTK is 48 bytes: KCK (16) + KEK (16) + TK (16).
 * Returns true on success, false on error.
 */
bool wpa_ptk_compute(const uint8_t pmk[32],
                      const uint8_t anonce[32], const uint8_t snonce[32],
                      const uint8_t ap_mac[6], const uint8_t sta_mac[6],
                      uint8_t out_ptk[48]);

/* Verify the MIC in an EAPOL frame given the PTK.
 * ptk[0..15] is the KCK (Key Confirmation Key) for MIC computation.
 * eapol_frame should have the MIC field zeroed for comparison.
 * For WPA (802.1X), use HMAC-MD5; for WPA2, use HMAC-SHA1.
 * is_sha1: false = HMAC-MD5 (WPA), true = HMAC-SHA1 (WPA2)
 * Returns true if MIC matches, false otherwise.
 */
bool wpa_mic_verify(const uint8_t ptk[48], const uint8_t eapol_frame[/*eapol_len*/],
                     size_t eapol_len, const uint8_t captured_mic[16],
                     bool is_sha1);
