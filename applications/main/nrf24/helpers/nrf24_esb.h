/* Shared Enhanced-ShockBurst (ESB) promiscuous-frame decode -- the
 * validate+extract half of nrf24_mj_core.c's mj_fingerprint(), factored out
 * of scene_sniffer.c / scene_keysniffer.c so the two attack scenes share one
 * copy (mj_fingerprint() bundles CRC-validate + vendor-match with no way to
 * get just the validate+extract half).
 *
 * PRIVATE-only: the sole implementation (nrf24_esb.c) is guarded
 * #ifndef PUBLIC_BUILD, since its only callers are the private attack scenes.
 *
 * Pure logic, furi-free -- host-testable with -Iapplications/main/nrf24/helpers.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Decode one raw promiscuous nRF24 frame: try both preamble alignments,
 * validate the CRC16-CCITT, and on success extract the 5-byte address and the
 * payload. Returns false (leaving out_* untouched) on any invalid/short frame.
 *
 *   raw_buf     : captured bytes (read up to min(size,37))
 *   size        : raw_buf length; frames shorter than 10 bytes are rejected
 *   out_addr    : caller-provided 5-byte buffer (written only on success)
 *   out_payload : caller-provided 32-byte buffer (written only on success)
 *   out_len     : receives the payload length in [1,32] (written only on success)
 */
bool nrf24_esb_decode_frame(
    const uint8_t* raw_buf,
    uint8_t size,
    uint8_t* out_addr,
    uint8_t* out_payload,
    uint8_t* out_len);
