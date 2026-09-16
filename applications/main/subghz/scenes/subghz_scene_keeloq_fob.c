#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "lib/subghz/blocks/generic.h"
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/protocols/vag.h>
#include <lib/subghz/protocols/keeloq.h>

#define TAG "SubGhzKeeloqFob"

// Default on-demand resync leap for VAG / generic car fobs when the user hasn't dialled in a
// jump. Must exceed how far the receiver drifted (real-fob presses since) to re-lock; small
// enough that the real key catches back up in a reasonable number of presses. User-tunable
// via the SetJump scene (keeloq_clone_jump).
#define SUBGHZ_FOB_SYNC_DEFAULT_MARGIN 256u

// Bridges the fob View's callback events onto the scene manager.
static void subghz_scene_keeloq_fob_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

// Refresh the on-screen counter from fff_data after TX. Both the KeeLoq clone and the
// VAG encoder advance AND persist the counter into fff_data during subghz_tx_start, so
// we only read the post-TX value here — no manual +1 (that would double-advance VAG).
static void subghz_scene_keeloq_fob_refresh_cnt(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    uint32_t cnt = 0;
    flipper_format_rewind(fff);
    flipper_format_read_uint32(fff, "Cnt", &cnt, 1);
    subghz_keeloq_fob_set_sent(subghz->subghz_keeloq_fob, cnt);
}

// Persist the post-TX counter to the backing .sub on disk so a reopened clone resumes
// forward instead of replaying already-accepted counters — a same-serial clone shares the
// receiver's counter slot, so a stale reload would be rejected until it catches up. Only
// for Garage-opened entries (scene state 1) with a known file_path; a freshly-built clone
// is in-RAM until the user Saves it, so we must never write to a stale path. The whole
// fff_data (already advanced + persisted in RAM by the encoder) is written via the standard
// save helper, so it works for KeeLoq clones, VAG, and generic car fobs alike.
static void subghz_scene_keeloq_fob_persist(SubGhz* subghz) {
    if(scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneKeeloqFob) != 1) return;
    if(furi_string_empty(subghz->file_path)) return;
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    subghz_save_protocol_to_file(subghz, fff, furi_string_get_cstr(subghz->file_path));
}

// Generic-protocol counter advance+persist after a successful TX. The car
// encoders split into two families and this handles both WITHOUT a per-protocol
// self-advance table:
//   * pp_encoder_read_fields protocols (Kia/Ford/... and Honda V7/V8/V9) read
//     their counter to rebuild the frame but do NOT persist an advance, so the
//     scene owns the roll: +1 and write it back.
//   * a few encoders self-advance AND persist their own counter during
//     subghz_tx_start (Honda V5, exactly like VAG/KeeLoq) — advancing again here
//     would double-step.
// The counter lives in the protocol's own field (keeloq_fob_cnt_field — "Cnt"
// for most, "Rolling"/"Counter"/"RollingCode" for the rolling families), NOT a
// hardcoded "Cnt". We read that field: if the encoder already moved it past the
// value we transmitted with (keeloq_fob_cnt_seed), adopt+display that; otherwise
// +1 and persist so the next press is the next frame. fff_data is the in-memory
// format the encoder reads each press; the on-disk .sub is unchanged until a
// re-Save. Only called on TX success.
static void subghz_scene_keeloq_fob_advance_cnt_generic(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    const char* field = subghz->keeloq_fob_cnt_field;
    uint32_t now = subghz->keeloq_fob_cnt_seed; // fallback if the field is absent
    flipper_format_rewind(fff);
    flipper_format_read_uint32(fff, field, &now, 1);
    uint32_t next;
    if(now != subghz->keeloq_fob_cnt_seed) {
        // Encoder self-advanced + persisted its own counter — just adopt it.
        next = now;
    } else {
        // Encoder read but did not persist — the scene owns the roll.
        next = now + 1;
        flipper_format_insert_or_update_uint32(fff, field, &next, 1);
    }
    subghz->keeloq_fob_cnt_seed = next;
    subghz_keeloq_fob_set_sent(subghz->subghz_keeloq_fob, next);
}

// Generic supported-protocol path: any car fob with an enumerable button table
// (Kia/Ford/Honda/Mazda/Subaru/...) reached from the Garage (saved) or the
// receiver "Emulate" (captured). fff_data already holds the loaded .sub-format
// frame (subghz_key_load / a stream copy). Reads the protocol's own button + Cnt,
// pre-seeds the counter with the desync-safe jump, and opens the shared grid.
// Returns true if it consumed the entry (VAG/KeeLoq fall through when false).
static bool subghz_scene_keeloq_fob_enter_generic(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    FuriString* proto = furi_string_alloc();
    flipper_format_rewind(fff);
    bool have_proto = flipper_format_read_string(fff, "Protocol", proto);
    const char* proto_cstr = furi_string_get_cstr(proto);

    size_t n = 0;
    const SubGhzFobButton* buttons = have_proto ? subghz_fob_buttons(proto_cstr, &n) : NULL;

    // Single-button "resend rolling" clone for synth-capable protocols that have
    // NO multi-button table (Kia V5/PSA/Fiat V0). Build a 1-tile table whose code
    // is the CAPTURED button, so the on-air button always matches the capture
    // (works whether or not the encoder reads Btn) while the counter still rolls
    // +1 per press. Static-lifetime buffer: the view stores the pointer, only one
    // emulate runs at a time.
    static SubGhzFobButton single_tile[1];
    if((!buttons || n == 0) && have_proto && subghz_fob_single_button_synth(proto_cstr)) {
        uint32_t cap_btn = 0;
        flipper_format_rewind(fff);
        flipper_format_read_uint32(fff, subghz_fob_button_field(proto_cstr), &cap_btn, 1);
        single_tile[0].code = (uint8_t)cap_btn;
        single_tile[0].name = "Send";
        buttons = single_tile;
        n = 1;
    }

    if(!buttons || n == 0) {
        // Provider flagged this supported at routing time but has no table now — bail
        // to the previous scene rather than show an empty grid.
        FURI_LOG_E(TAG, "Generic fob: no button table for '%s'", have_proto ? proto_cstr : "?");
        furi_string_free(proto);
        scene_manager_previous_scene(subghz->scene_manager);
        return true;
    }
    if(n > SUBGHZ_KEELOQ_FOB_MAX_TILES) {
        FURI_LOG_W(
            TAG,
            "Fob table '%s' has %u buttons; showing first %u",
            proto_cstr,
            (unsigned)n,
            (unsigned)SUBGHZ_KEELOQ_FOB_MAX_TILES);
    }

    // Field this protocol's encoder reads the button from ("Btn"/"Button"/"BtnLo").
    const char* field = subghz_fob_button_field(proto_cstr);
    strncpy(subghz->keeloq_fob_field, field, sizeof(subghz->keeloq_fob_field) - 1);
    subghz->keeloq_fob_field[sizeof(subghz->keeloq_fob_field) - 1] = '\0';
    // Counter field this protocol's encoder reads ("Cnt" for most; "Rolling"/
    // "Counter"/"RollingCode" for the SantaFe / Honda rolling families). The
    // press path advances THIS field so the on-air rolling code actually changes.
    const char* cnt_field = subghz_fob_counter_field(proto_cstr);
    strncpy(subghz->keeloq_fob_cnt_field, cnt_field, sizeof(subghz->keeloq_fob_cnt_field) - 1);
    subghz->keeloq_fob_cnt_field[sizeof(subghz->keeloq_fob_cnt_field) - 1] = '\0';
    // Static-lifetime title for the view: protocol name + emulate mode, so the
    // operator sees this is a desync-safe rolling "Second fob" (counter +1/press).
    snprintf(
        subghz->keeloq_fob_title,
        sizeof(subghz->keeloq_fob_title),
        "%s \xB7 %s",
        proto_cstr,
        subghz_fob_emulate_kind_str(subghz_fob_emulate_kind(proto_cstr)));

    uint32_t captured_btn = 0, cnt_file = 0;
    flipper_format_rewind(fff);
    // Distinguish "button field absent" (mark no tile) from a genuine 0x0 capture
    // (Subaru Lock / Honda Panic) so a 0x0-code table isn't falsely flagged as
    // the captured button.
    bool have_btn = flipper_format_read_uint32(fff, field, &captured_btn, 1);
    flipper_format_rewind(fff);
    flipper_format_read_uint32(fff, cnt_field, &cnt_file, 1); // absent -> 0 (graceful)

    // Reuse the no-desync jump: pre-seed the protocol's counter so the first press
    // is a future frame.
    uint16_t jump = subghz->keeloq_clone_jump; // 0 = desync-safe default
    uint32_t cnt_start = cnt_file + jump;
    flipper_format_insert_or_update_uint32(fff, cnt_field, &cnt_start, 1);
    subghz->keeloq_fob_cnt_seed = cnt_start;

    uint8_t real_btn = have_btn ? (uint8_t)captured_btn : SUBGHZ_KEELOQ_FOB_NO_BTN;

    subghz_keeloq_fob_set_callback(
        subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
    subghz_keeloq_fob_set_table(
        subghz->subghz_keeloq_fob, buttons, n, real_btn, cnt_start, jump);
    // Title AFTER set_table: set_table resets the title to the "Fob Emulator"
    // default, so the protocol-name title must be applied last or it is lost.
    subghz_keeloq_fob_set_title(subghz->subghz_keeloq_fob, subghz->keeloq_fob_title);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);

    furi_string_free(proto);
    return true;
}

void subghz_scene_keeloq_fob_on_enter(void* context) {
    SubGhz* subghz = context;

    // Persist the advancing counter to disk only for clones opened from the Garage (they
    // have a backing .sub at subghz->file_path). A freshly-built clone is in-RAM until the
    // user Saves it, so scene-state 0 keeps it from writing to a stale path.
    scene_manager_set_scene_state(
        subghz->scene_manager, SubGhzSceneKeeloqFob, subghz->keeloq_fob_from_garage ? 1 : 0);

    // Generic multi-button path (every supported car protocol other than the
    // VAG/KeeLoq special cases below). The routing scene set keeloq_fob_generic +
    // keeloq_fob_from_garage and loaded the .sub into fff_data. Keep the generic
    // flag set for the press path; consume from_garage like the VAG/KeeLoq paths.
    if(subghz->keeloq_fob_generic) {
        subghz->keeloq_fob_from_garage = false;
        subghz_scene_keeloq_fob_enter_generic(subghz);
        return;
    }

    // Desync-safe default: NO forward lead (jump stays 0). A clone advances the shared
    // rolling counter by +1 like the real fob, so the original never gets stranded — its
    // worst-case catch-up is only as many presses as the clone was used. (Was 100; that
    // +100 leap is exactly what desynced the original fob. The SetJump scene can still
    // opt into a small lead for reliability at the cost of a few original catch-up presses.)

    // Detect the loaded frame's protocol, but ONLY on a from_garage entry. VAG is
    // always reached via Save->Garage. A fresh KeeLoq clone flow (keeloq_clone_flow,
    // from_garage=false) reaches this scene with STALE fff_data that may still hold
    // "Protocol: VAG" from a prior VAG emulate; reading it there would mis-route the
    // KeeLoq clone into the VAG branch and skip gen_keeloq_clone. KeeLoq Garage entries
    // are also from_garage but carry a fresh "Protocol: KeeLoq", so the string check
    // keeps them on the KeeLoq path below.
    bool is_vag = false;
    if(subghz->keeloq_fob_from_garage) {
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        FuriString* proto = furi_string_alloc();
        flipper_format_rewind(fff);
        if(flipper_format_read_string(fff, "Protocol", proto) &&
           furi_string_equal_str(proto, VAG_PROTOCOL_NAME)) {
            is_vag = true;
        }
        furi_string_free(proto);
    }

    if(is_vag) {
        subghz->keeloq_fob_from_garage = false; // consume the flag
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        uint32_t btn = 0, cnt_file = 0;
        flipper_format_rewind(fff);
        flipper_format_read_uint32(fff, "Btn", &btn, 1);
        flipper_format_rewind(fff);
        flipper_format_read_uint32(fff, "Cnt", &cnt_file, 1);

        // Normalize VAG button code to the low-nibble form (0x1/0x2/0x4) the fob
        // tiles use; some captures carry the 0x10/0x20/0x40 alternate encoding.
        uint8_t rb = (uint8_t)btn;
        if(rb & 0x70) rb >>= 4;
        rb &= 0x07;

        // Desync-safe: no forward lead by default (vjump 0) — start at the captured
        // counter and let the encoder advance +1 per press, so the original recovers in
        // 1-2 presses. KeyIdx/Type stay in fff; the encoder re-encrypts from Cnt/Btn.
        uint16_t vjump = subghz->keeloq_clone_jump; // 0 = no lead
        uint32_t cnt_start = cnt_file + vjump;
        flipper_format_update_uint32(fff, "Cnt", &cnt_start, 1);

        subghz_keeloq_fob_set_callback(
            subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
        subghz_keeloq_fob_set_state(
            subghz->subghz_keeloq_fob, SubGhzFobKindVag, rb, cnt_start, vjump);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
        return;
    }

    uint8_t real_btn;
    uint16_t cnt;
    uint16_t jump;

    if(subghz->keeloq_fob_from_garage) {
        // Re-opened from the Garage: fff_data already holds the loaded CloneMode .sub.
        subghz->keeloq_fob_from_garage = false;
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        uint32_t v = 0;
        flipper_format_rewind(fff);
        if(!(flipper_format_read_uint32(fff, "CloneMode", &v, 1) && v == 1)) {
            furi_string_set(subghz->error_str, "Not a saved fob\n(no CloneMode).");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return;
        }
        flipper_format_rewind(fff);
        v = 0;
        flipper_format_read_uint32(fff, "Btn", &v, 1);
        real_btn = (uint8_t)(v & 0x0F);
        flipper_format_rewind(fff);
        v = 0;
        flipper_format_read_uint32(fff, "Cnt", &v, 1);
        cnt = (uint16_t)v;
        flipper_format_rewind(fff);
        v = subghz->keeloq_clone_jump;
        flipper_format_read_uint32(fff, "CloneJump", &v, 1);
        jump = (uint16_t)v;
    } else {
        // Build the clone into fff_data from the gen_info the Clone action set. BFT/Erreka
        // (non-zero seed) route through the seed-aware builder so the derived device key is
        // correct; normal KeeLoq uses the no-desync clone builder.
        bool ok =
            subghz->gen_info->keeloq.seed ?
                subghz_txrx_gen_keeloq_bft_protocol(
                    subghz->txrx,
                    subghz->gen_info->mod,
                    subghz->gen_info->freq,
                    subghz->gen_info->keeloq.serial,
                    subghz->gen_info->keeloq.btn,
                    subghz->gen_info->keeloq.cnt,
                    subghz->gen_info->keeloq.seed,
                    subghz->gen_info->keeloq.manuf) :
                subghz_txrx_gen_keeloq_clone(
                    subghz->txrx,
                    subghz->gen_info->mod,
                    subghz->gen_info->freq,
                    subghz->gen_info->keeloq.serial,
                    subghz->gen_info->keeloq.btn,
                    subghz->gen_info->keeloq.cnt,
                    subghz->keeloq_clone_jump,
                    subghz->gen_info->keeloq.manuf);
        subghz->keeloq_clone_flow = false;
        if(!ok) {
            furi_string_set(
                subghz->error_str, "Function requires\nan SD card with\nfresh databases.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return;
        }
        real_btn = (uint8_t)(subghz->gen_info->keeloq.btn & 0x0F);
        cnt = (uint16_t)subghz->gen_info->keeloq.cnt;
        jump = subghz->keeloq_clone_jump;
    }

    subghz_keeloq_fob_set_callback(
        subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
    subghz_keeloq_fob_set_state(
        subghz->subghz_keeloq_fob, SubGhzFobKindKeeloq, real_btn, cnt, jump);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
}

bool subghz_scene_keeloq_fob_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewKeeloqFobSend) {
            // Ignore new presses while a burst is still on air.
            if(subghz->state_notifications == SubGhzNotificationStateTx) return true;
            uint8_t nibble = subghz_keeloq_fob_selected_nibble(subghz->subghz_keeloq_fob);
            uint32_t btn = nibble;
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            // Generic protocols write the button into their OWN field ("Btn"/"Button"
            // /"BtnLo"); the encoder rebuilds the frame from it. Cnt was pre-seeded in
            // on_enter and advances after each successful TX (below). VAG/KeeLoq keep
            // the legacy "Btn" write (their encoder self-advances the counter).
            const char* btn_field = subghz->keeloq_fob_generic ? subghz->keeloq_fob_field : "Btn";
            flipper_format_insert_or_update_uint32(fff, btn_field, &btn, 1);
            subghz_txrx_hopper_pause(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            // Ordinary clone press: one frame at the next counter (+1), encoder repeat=3.
            // (Long-press OK = Sync, which arms the KeeLoq (N,N+1) force-resync pair below.)
            subghz_block_generic_global.endless_tx = false;
            if(subghz_tx_start(subghz, fff)) {
                subghz->state_notifications = SubGhzNotificationStateTx;
            } else {
                subghz_txrx_rx_start(subghz->txrx);
                subghz_txrx_hopper_unpause(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
            }
            return true;
        } else if(event.event == SubGhzCustomEventViewKeeloqFobSync) {
            // Long-press OK = FORCE-RESYNC: recover a rolling remote whose receiver counter
            // drifted forward (the real fob was used since). Two mechanisms by protocol:
            //  - KeeLoq clone: arm the encoder's double-window (N, N+1) pair jumped ahead by
            //    `margin` — the receiver re-locks on the pair's 2nd frame.
            //  - VAG / generic car fob: jump THIS remote's rolling counter forward by `margin`
            //    and send one frame at the new value, landing in the receiver's resync window
            //    ahead of its stored counter (the on-demand form of the existing "jump" lead;
            //    the VAG 4th tile advertises it).
            if(subghz->state_notifications == SubGhzNotificationStateTx) return true;
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            FuriString* proto = furi_string_alloc();
            flipper_format_rewind(fff);
            bool is_keeloq = flipper_format_read_string(fff, "Protocol", proto) &&
                             furi_string_equal_str(proto, SUBGHZ_PROTOCOL_KEELOQ_NAME);
            furi_string_free(proto);
            uint32_t clone_mode = 0;
            flipper_format_rewind(fff);
            flipper_format_read_uint32(fff, "CloneMode", &clone_mode, 1);

            uint8_t nibble = subghz_keeloq_fob_selected_nibble(subghz->subghz_keeloq_fob);
            uint32_t btn = nibble;

            if(is_keeloq && clone_mode == 1 && !subghz->keeloq_fob_generic) {
                // KeeLoq clone: (N, N+1) double-window resync pair via the encoder.
                flipper_format_insert_or_update_uint32(fff, "Btn", &btn, 1);
                uint16_t margin =
                    (subghz->keeloq_clone_jump > 16 && subghz->keeloq_clone_jump < 0x8000) ?
                        (uint16_t)subghz->keeloq_clone_jump :
                        0; // 0 -> encoder's built-in default
                subghz_protocol_keeloq_encoder_request_resync(margin);
            } else {
                // VAG / generic car fob: jump the rolling counter forward, then send one frame.
                uint32_t margin = subghz->keeloq_clone_jump ? subghz->keeloq_clone_jump :
                                                              SUBGHZ_FOB_SYNC_DEFAULT_MARGIN;
                const char* cnt_field =
                    subghz->keeloq_fob_generic ? subghz->keeloq_fob_cnt_field : "Cnt";
                const char* btn_field =
                    subghz->keeloq_fob_generic ? subghz->keeloq_fob_field : "Btn";
                uint32_t cur = 0;
                flipper_format_rewind(fff);
                flipper_format_read_uint32(fff, cnt_field, &cur, 1);
                uint32_t target = cur + margin;
                flipper_format_insert_or_update_uint32(fff, cnt_field, &target, 1);
                // Keep the generic post-TX advance in step with the jump so the next ordinary
                // press is target+1 (VAG self-advances + is read back via refresh_cnt, so this
                // assignment is harmless for it).
                subghz->keeloq_fob_cnt_seed = target;
                flipper_format_insert_or_update_uint32(fff, btn_field, &btn, 1);
            }

            subghz_txrx_hopper_pause(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_block_generic_global.endless_tx = false;
            if(subghz_tx_start(subghz, fff)) {
                subghz->state_notifications = SubGhzNotificationStateTx;
            } else {
                subghz_txrx_rx_start(subghz->txrx);
                subghz_txrx_hopper_unpause(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
            }
            return true;
        } else if(event.event == SubGhzCustomEventViewKeeloqFobBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz->state_notifications == SubGhzNotificationStateTx) {
            if(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) {
                // still sending
                return true;
            }
            // burst finished
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_stop(subghz->txrx);
            if(subghz->keeloq_fob_generic) {
                // Generic encoders don't self-advance the counter; the scene does.
                subghz_scene_keeloq_fob_advance_cnt_generic(subghz);
            } else {
                // VAG/KeeLoq: their encoder already advanced + persisted Cnt.
                subghz_scene_keeloq_fob_refresh_cnt(subghz);
            }
            // Write the advanced counter through to the backing .sub (Garage clones only),
            // so reopening the clone resumes forward instead of replaying stale counters.
            subghz_scene_keeloq_fob_persist(subghz);
        }
        return true;
    }
    return false;
}

void subghz_scene_keeloq_fob_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_stop(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    subghz->keeloq_clone_flow = false;
    subghz->keeloq_fob_generic = false; // clear the generic-path flag for the next entry
}
