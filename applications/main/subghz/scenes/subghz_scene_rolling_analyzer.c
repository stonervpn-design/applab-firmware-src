#include "../subghz_i.h"

#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "../helpers/subghz_rollback.h"
#include <lib/subghz/blocks/generic.h>

#define TAG "SubGhzSceneRollingAnalyzer"

// Read-only Rolling-Code Analyzer.
//
// Mirrors the receiver-info scene's structure but renders a structured summary
// of an already-decoded DYNAMIC (rolling-code) capture: protocol, manufacturer,
// serial, button, counter, bit length, plus a plain-language verdict. It does
// NOT transmit, re-decode, or mutate the capture — it only reads the fields the
// decoder already produced.
//
// The generic block deserialize (subghz_block_generic_deserialize) only recovers
// `data` + `data_count_bit` from the saved .sub; the serial / button / counter /
// manufacturer are derived by the protocol decoder at decode time and surfaced
// through subghz_protocol_decoder_base_get_string(). For the KeeLoq family that
// string carries "Fix:0x........" (fixed code = btn<<28 | serial), "Cnt:XXXX"
// (or "Cnt:????" when no manufacturer key matched) and "MF:<name>" (or
// "MF:Unknown"). We parse those authoritative values rather than re-deriving them.

// Field parsing (hex-after / MF: extraction) is shared with the RollBack feature
// via the host-tested pure core in subghz_rollback_core.c — see
// subghz_rollback_hex_after() / subghz_rollback_manufacturer().

void subghz_scene_rolling_analyzer_on_enter(void* context) {
    SubGhz* subghz = context;

    widget_reset(subghz->widget);

    // Re-load + deserialize the selected capture so the analyzer is self-contained
    // and does not depend on the receiver's live decoder state (mirrors how the
    // receiver-info scene re-parses before it acts). Read-only: no TX, no mutation.
    bool loaded = subghz_txrx_load_decoder_by_name_protocol(
        subghz->txrx,
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
    FlipperFormat* fff = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    if(loaded && fff) {
        subghz_protocol_decoder_base_deserialize(subghz_txrx_get_decoder(subghz->txrx), fff);
    }

    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);

    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Rolling Code");

    if(!decoder || !decoder->protocol || !fff) {
        widget_add_string_element(
            subghz->widget, 0, 16, AlignLeft, AlignTop, FontSecondary, "No capture to analyze.");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    // Structured fields the saved capture carries directly.
    SubGhzBlockGeneric generic = {0};
    subghz_block_generic_deserialize(&generic, fff);
    uint16_t data_count_bit = generic.data_count_bit;

    // Authoritative decoded text (serial / counter / manufacturer live here).
    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, text);
    const char* decoded_text = furi_string_get_cstr(text);

    const char* protocol_name = decoder->protocol->name;

    char manufacturer[24] = {0};
    bool has_mf = subghz_rollback_manufacturer(decoded_text, manufacturer, sizeof(manufacturer));

    // "decoded" = a manufacturer key matched: MF present and not "Unknown", and
    // the counter was not left as the "????" not-decodable placeholder.
    bool decoded = has_mf && (strstr(decoded_text, "MF:Unknown") == NULL) &&
                   (strstr(decoded_text, "Cnt:????") == NULL);

    uint32_t fix = subghz_rollback_hex_after(decoded_text, "Fix:");
    uint32_t serial = fix & 0x0FFFFFFF;
    uint8_t btn = (uint8_t)((fix >> 28) & 0x0F);
    uint32_t cnt = subghz_rollback_hex_after(decoded_text, "Cnt:");

    FuriString* body = furi_string_alloc();
    furi_string_cat_printf(body, "Proto: %s\n", protocol_name ? protocol_name : "?");
    furi_string_cat_printf(body, "MF: %s\n", decoded ? manufacturer : "key unknown");
    furi_string_cat_printf(body, "SN:0x%08lX  Btn:%d\n", (unsigned long)serial, (int)btn);
    furi_string_cat_printf(
        body, "Cnt:0x%04lX (%lu)\n", (unsigned long)cnt, (unsigned long)cnt);
    furi_string_cat_printf(body, "Bits:%d", (int)data_count_bit);

    widget_add_string_multiline_element(
        subghz->widget, 0, 13, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(body));

    const char* verdict =
        decoded ?
            "Rolling: raw replay rejected; TX re-advances counter." :
            "Rolling: mfr key unknown - counter not decodable. Add key to keeloq_mfcodes_user.";
    widget_add_string_multiline_element(
        subghz->widget, 0, 51, AlignLeft, AlignTop, FontSecondary, verdict);

    furi_string_free(body);
    furi_string_free(text);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_rolling_analyzer_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    // Read-only view: Back is handled by the scene manager (returns to receiver-info).
    return false;
}

void subghz_scene_rolling_analyzer_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
