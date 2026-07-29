#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * NTLMv2 Response Computation
 * Computes the NTLMv2 response: HMAC-MD5(NTLMv2-Hash, challenge || context)
 * where NTLMv2-Hash = HMAC-MD5(MD4(UTF16-LE(password)), UPPER(domain) || user)
 */
void ntlmv2_response_compute(const char* password, const char* username, const char* domain,
                              const uint8_t challenge[8], const uint8_t context[8],
                              uint8_t out_response[16]);

/**
 * NTLM Type-3 Message Parsed Fields
 * Extracted from NTLM Type-3 message (binary format)
 */
typedef struct {
    char username[256];
    char domain[256];
    uint8_t challenge[8];
    uint8_t response[16];  // the v2 response (HMAC-MD5 blob)
} NtlmV2Parsed;

/**
 * Parse NTLM Type-3 Message
 * Bounds-checked extraction of username, domain, challenge, and response
 * Returns true on success, false on parse error or buffer over-read
 */
bool ntlm_parse_type3(const uint8_t* frame, size_t len, NtlmV2Parsed* out);

/**
 * Format Captured NetNTLMv2 for Hashcat Mode 5500
 * Output format: user::domain:serverchallenge:NTProofStr:blob
 * Returns the number of bytes written (excluding null terminator), 0 on error
 */
size_t net_ntlm_hashcat_5500(const NtlmV2Parsed* parsed, char* out, size_t out_max);
