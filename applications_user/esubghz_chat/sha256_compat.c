/* SHA-256 shim backed by ESP-IDF mbedTLS (already in the build). */
#include "sha256_compat.h"
#include "mbedtls/sha256.h"

void sha256(const uint8_t* data, size_t len, uint8_t* output) {
    /* last arg 0 => SHA-256 (not SHA-224). mbedtls 3.x signature. */
    mbedtls_sha256(data, len, output, 0);
}
