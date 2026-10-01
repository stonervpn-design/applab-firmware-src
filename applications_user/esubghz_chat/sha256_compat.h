#pragma once
/* Flipper toolbox/sha256.h compatibility shim for the ESP32 port.
 * The port has no lib/toolbox/sha256; wrap ESP-IDF mbedTLS instead. */
#include <stddef.h>
#include <stdint.h>

/* One-shot SHA-256: hash `len` bytes at `data` into the 32-byte `output`. */
void sha256(const uint8_t* data, size_t len, uint8_t* output);
