#pragma once

#include <stdint.h>

// Doorbell Brute — standalone Sub-GHz sweep for wireless doorbells.
// Replays a table of real Princeton/PT2262-family doorbell chime codes
// (doorbell_codes.h, from the FlipperZero-Subghz-DB "Doorbells" set) as properly
// framed Princeton packets via the SubBrute worker's manual-transmit path, so an
// actual bell decodes a frame and rings. (A raw de Bruijn burst does not — it has
// no PWM symbols or sync gap, so a Princeton receiver never decodes it.)
// Authorized-testing tool: use only on doorbells you own or are permitted to test.

int32_t doorbell_brute_app(void* p);
