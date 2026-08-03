#include "../subghz_i.h"

#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "../helpers/subghz_rollback.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/keeloq.h>
#include <string.h>

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

// Stable storage for the manufacturer name pulled off a decoded capture.
// GenInfo.keeloq.manuf is a `const char*` that must outlive this scene, and the
// KeeLoq decoder's own manufacture_name pointer (private to keeloq.c — the struct
// is defined only in that .c file, not exposed via keeloq.h) can be reused/
// mutated by the next decode. Mirrors subghz_scene_keeloq_mfr.c's
// subghz_keeloq_mfr_selected buffer.
static char subghz_scene_rolling_analyzer_clone_manuf[24];

// Clone-from-capture gate, built from the same decoded-text markers this scene
// already parses for its own display (Fix:/Cnt:/MF:) — see the file banner above
// for why those markers, not the decoder's private fields, are the accessor.
// True only for a KeeLoq rolling capture whose manufacturer key matched (MF !=
// "Unknown") and whose counter was actually recovered via the literal "Cnt:"
// marker.
//
// Two manufacturer families are excluded even though they otherwise look
// "decoded" (has_mf true, MF != "Unknown"):
//   - "BFT" / "Erreka" (SECURE learning). keeloq.c's get_string() format for
//     these is "...MF:%s Sd:%08lX" with NO separator between the name and
//     " Sd:", so subghz_rollback_manufacturer() would capture "BFT Sd:1A2B3C4D"
//     as one string and an exact strcmp("BFT") would silently fail to exclude
//     it. Guard on the raw text ("MF:BFT" / "MF:Erreka") instead, the same way
//     "MF:Unknown" is already checked below. These mfrs also need a
//     programming-mode seed a normal button press never carries, so cloning
//     from a capture isn't possible for them regardless (no-desync-clone design
//     spec: seed-only mfrs).
//   - "AN-Motors": its get_string() branch prints "PsCn:%04lX" (pseudo-counter),
//     never the literal "Cnt:" marker, so subghz_rollback_hex_after(t, "Cnt:")
//     would silently return 0 instead of a real value. Requiring "Cnt:" to be
//     present excludes it along with anything else lacking a real recovered
//     counter.
static bool
    subghz_scene_rolling_analyzer_can_clone(const char* protocol_name, const char* decoded_text) {
    if(!protocol_name || strcmp(protocol_name, SUBGHZ_PROTOCOL_KEELOQ_NAME) != 0) return false;
    if(!decoded_text) return false;
    return strstr(decoded_text, "MF:Unknown") == NULL &&
           strstr(decoded_text, "MF:BFT") == NULL && strstr(decoded_text, "MF:Erreka") == NULL &&
           strstr(decoded_text, "Cnt:") != NULL && strstr(decoded_text, "Cnt:????") == NULL;
}

static void subghz_scene_rolling_analyzer_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if((result == GuiButtonTypeCenter) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollingAnalyzerClone);
    }
}

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

    // Only slot in use above is text; all three button slots are free here, so
    // no collision risk (unlike receiver-info's widget, whose Left/Center/Right
    // are already spoken for by ID/Send/Save on every KeeLoq capture).
    if(subghz_scene_rolling_analyzer_can_clone(protocol_name, decoded_text)) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Clone",
            subghz_scene_rolling_analyzer_button_callback,
            subghz);
    }

    furi_string_free(body);
    furi_string_free(text);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_rolling_analyzer_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneRollingAnalyzerClone) {
        // Stop RX/hopper before handing off — SetJump's byte-input result calls
        // subghz_txrx_gen_keeloq_clone() and routes to the Transmitter, so the
        // radio needs to be idle (mirrors receiver-info's Save handler).
        subghz->state_notifications = SubGhzNotificationStateIDLE;
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
        subghz_txrx_stop(subghz->txrx);

        // Re-load + deserialize the selected capture fresh: the RX hopper may
        // have reassigned the shared decoder to a different capture since this
        // screen was drawn (on_enter loaded it but did not stop RX — it's a
        // read-only view, same as this scene's own header comment notes).
        bool loaded = subghz_txrx_load_decoder_by_name_protocol(
            subghz->txrx,
            subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
        FlipperFormat* fff =
            subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
        SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
        if(loaded && fff && decoder) {
            subghz_protocol_decoder_base_deserialize(decoder, fff);
        }
        if(!loaded || !fff || !decoder || !decoder->protocol) {
            return false;
        }

        FuriString* text = furi_string_alloc();
        subghz_protocol_decoder_base_get_string(decoder, text);
        const char* t = furi_string_get_cstr(text);

        if(!subghz_scene_rolling_analyzer_can_clone(decoder->protocol->name, t)) {
            furi_string_free(text);
            return false; // guard: never seed gen_info from an ungated capture
        }

        uint32_t fix = subghz_rollback_hex_after(t, "Fix:");
        uint32_t cnt = subghz_rollback_hex_after(t, "Cnt:");
        char mf[24] = {0};
        subghz_rollback_manufacturer(t, mf, sizeof(mf));
        furi_string_free(text);

        strncpy(
            subghz_scene_rolling_analyzer_clone_manuf,
            mf,
            sizeof(subghz_scene_rolling_analyzer_clone_manuf) - 1);
        subghz_scene_rolling_analyzer_clone_manuf
            [sizeof(subghz_scene_rolling_analyzer_clone_manuf) - 1] = '\0';

        SubGhzRadioPreset* preset =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);

        subghz->gen_info->type = GenKeeloq;
        subghz->gen_info->mod = "AM650"; // KeeLoq is AM-only (SubGhzProtocolFlag_AM in keeloq.c)
        subghz->gen_info->freq = preset ? preset->frequency : 433920000;
        subghz->gen_info->keeloq.serial = fix & 0x0FFFFFFF;
        subghz->gen_info->keeloq.btn = (uint8_t)((fix >> 28) & 0x0F);
        subghz->gen_info->keeloq.cnt = (uint16_t)cnt;
        subghz->gen_info->keeloq.manuf = subghz_scene_rolling_analyzer_clone_manuf;

        subghz->keeloq_clone_flow = true;
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
        return true;
    }
    // Otherwise read-only: Back is handled by the scene manager (returns to receiver-info).
    return false;
}

void subghz_scene_rolling_analyzer_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
