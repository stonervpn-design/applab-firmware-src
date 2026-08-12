#include "../subghz_i.h"

#include "../helpers/subghz_txrx_i.h"
#include "../helpers/signal_fingerprint.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/psa.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <string.h>

#ifndef PUBLIC_BUILD
#include "../helpers/subghz_chip_id.h"
#include "../helpers/subbrute_protocols.h" // SubBruteAttacks enum for the guided-attack handoff
#include "../helpers/subghz_keeloq_registry.h" // family/generator classification (Task 1)
#include "../helpers/subghz_rollback.h" // subghz_rollback_hex_after/_manufacturer (MF: text parse)
#include <lib/subghz/protocols/keeloq.h> // SUBGHZ_PROTOCOL_KEELOQ_NAME
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h> // keystore iteration for the matched key's learning type
#endif

#define TAG "SubGhzSceneAnalyze"

// Signal-ID ("What is this?") scene — the identification hub.
//
// Reads the already-decoded capture the user selected and runs the pure, non-
// transmitting fingerprint engine (helpers/signal_fingerprint.c, logic verified
// by tools/signal_id/build_fingerprints.py) to surface make/model, rolling-vs-
// fixed, TX-capability and a suggested action. For rolling / PSA captures it
// offers a "Details" spoke that continues to the existing deeper tool
// (Rolling-Code Analyzer / PSA brute-force). Read-only: never transmits.

static bool subghz_scene_analyze_is_dynamic(SubGhz* subghz) {
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol) return false;
    return decoder->protocol->type == SubGhzProtocolTypeDynamic;
}

static bool subghz_scene_analyze_is_psa(SubGhz* subghz) {
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol || !decoder->protocol->name) return false;
    return strcmp(decoder->protocol->name, PSA_PROTOCOL_NAME) == 0;
}

#ifndef PUBLIC_BUILD
// True if the analyzed capture is a supported car fob that can open the multi-
// button rolling fob grid (transmittable + subghz_fob_can_emulate incl. the
// Honda-V3 / Fiat-V1 replay guards). The hub offers this as an "Emul" spoke so
// the receiver-info Left slot can stay the Signal-ID entry point ("ID") for
// every dynamic capture.
static bool subghz_scene_analyze_can_emulate(SubGhz* subghz) {
    if(!subghz_txrx_protocol_is_transmittable(subghz->txrx, false)) return false;
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol || !decoder->protocol->name) return false;
    FlipperFormat* raw = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    return subghz_fob_can_emulate(raw, decoder->protocol->name);
}

// Registry family lookup for a matched KeeLoq manufacturer name (Task 1's
// subghz_keeloq_registry_type_info needs the keystore's learning `type` to
// disambiguate the shared type 9/10/11 slots -- see subghz_keeloq_registry.c).
// Finds the matching keystore entry by name (mirrors subghz_scene_keeloq_
// console.c's keystore access) and classifies it. Falls back to a generic
// decode-only label when no exact keystore entry matches -- this is a display
// label only, never used to route a generate/attack action, so an imprecise
// fallback is safe.
static KeeloqTypeInfo subghz_scene_analyze_keeloq_family(SubGhz* subghz, const char* mf_name) {
    KeeloqTypeInfo info = {KlFamGeneric, GenKeeloq, false, true};
    if(!mf_name || !mf_name[0]) return info;
    SubGhzEnvironment* environment = subghz->txrx->environment;
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    if(!keystore) return info;
    SubGhzKeyArray_t* data = subghz_keystore_get_data(keystore);
    if(!data) return info;
    for M_EACH(key, *data, SubGhzKeyArray_t) {
        if(strcmp(furi_string_get_cstr(key->name), mf_name) == 0) {
            return subghz_keeloq_registry_type_info(key->type, mf_name);
        }
    }
    return info;
}

static const char* subghz_scene_analyze_family_label(KeeloqFamily f) {
    switch(f) {
    case KlFamFaacSLH:
        return "FAAC SLH";
    case KlFamKingGates:
        return "KingGates";
    case KlFamJarolift:
        return "Jarolift";
    case KlFamBeninca:
        return "Beninca ARC";
    case KlFamKiaCar:
        return "Kia/Hyundai";
    case KlFamOrphan:
        return "Orphan";
    case KlFamGeneric:
    default:
        return "KeeLoq";
    }
}

// Pull the manufacturer name off the decoder's own text (mirrors
// subghz_scene_rolling_analyzer.c's parse -- the decoder's manufacture_name
// field is private to keeloq.c, not exposed via keeloq.h, so the "MF:" marker
// in get_string() is the accessor). Returns true when a real (non-"Unknown")
// key match is present; on true, `mf_out` holds the bare keystore name (any
// BFT/Erreka " Sd:..." suffix stripped).
static bool subghz_scene_analyze_keeloq_mf(
    SubGhzProtocolDecoderBase* decoder,
    char* mf_out,
    size_t mf_out_size) {
    FuriString* text = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder, text);
    const char* t = furi_string_get_cstr(text);
    bool has_mf = subghz_rollback_manufacturer(t, mf_out, mf_out_size);
    bool mf_known = has_mf && (strstr(t, "MF:Unknown") == NULL);
    furi_string_free(text);
    if(mf_known) {
        char* sp = strchr(mf_out, ' '); // "MF:BFT Sd:.." -> bare keystore name
        if(sp) *sp = '\0';
    }
    return mf_known;
}

// Guided-attack handoff (PRIVATE). Resolved SubBrute attack for the current
// fixed-code capture, or -1. Set in on_enter, consumed in on_event and in the
// Right-button callback below (to pick Attack vs the KeeLoq dictionary-attack
// spoke -- the two are mutually exclusive per capture, see on_enter).
static int s_analyze_attack = -1;
#endif

static SignalMod subghz_scene_analyze_mod_of(const char* preset_name) {
    if(preset_name && strstr(preset_name, "FM")) return SIG_MOD_FM;
    if(preset_name && strstr(preset_name, "AM")) return SIG_MOD_AM;
    return SIG_MOD_UNK;
}

static const char* subghz_scene_analyze_action_label(SignalAction a) {
    switch(a) {
    case SIG_ACTION_ATTACK:
        return "red-team surface";
    case SIG_ACTION_DEFEND:
        return "alarm/defensive";
    case SIG_ACTION_EXPLAIN:
        return "known device";
    case SIG_ACTION_CAPTURE:
    default:
        return "capture more";
    }
}

static void subghz_scene_analyze_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if((result == GuiButtonTypeLeft) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneAnalyzeDetails);
    }
#ifndef PUBLIC_BUILD
    else if((result == GuiButtonTypeRight) && (type == InputTypeShort)) {
        // Right is mutually exclusive between the two spokes that use it (only
        // one is ever added for a given capture -- see on_enter): fixed-code
        // guided Attack (s_analyze_attack >= 0) vs rolling-KeeLoq dictionary
        // attack (the "KeyAtk" spoke).
        if(s_analyze_attack >= 0) {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubGhzCustomEventSceneAnalyzeAttack);
        } else {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubGhzCustomEventSceneAnalyzeDictAttack);
        }
    } else if((result == GuiButtonTypeCenter) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneAnalyzeEmulate);
    }
#endif
}

#ifndef PUBLIC_BUILD
typedef struct {
    uint32_t hz;
    int attack;
} SubGhzAnalyzeFreqAttack;

static int subghz_analyze_nearest(uint32_t freq, const SubGhzAnalyzeFreqAttack* t, size_t n) {
    int best = -1;
    uint32_t best_d = 0xFFFFFFFFu;
    for(size_t i = 0; i < n; i++) {
        uint32_t d = freq > t[i].hz ? freq - t[i].hz : t[i].hz - freq;
        if(d < best_d) {
            best_d = d;
            best = t[i].attack;
        }
    }
    return best;
}

// (chip family, bit width, capture freq) -> nearest SubBrute framed attack, or -1.
static int subghz_analyze_chip_to_attack(SubGhzChipFamily f, int32_t bits, uint32_t freq) {
    switch(f) {
    case SubGhzChipCame: {
        static const SubGhzAnalyzeFreqAttack t[] = {
            {303875000u, SubBruteAttackCAME12bit303}, {307000000u, SubBruteAttackCAME12bit307},
            {315000000u, SubBruteAttackCAME12bit315}, {330000000u, SubBruteAttackCAME12bit330},
            {433920000u, SubBruteAttackCAME12bit433}, {868350000u, SubBruteAttackCAME12bit868}};
        return subghz_analyze_nearest(freq, t, 6);
    }
    case SubGhzChipNice: {
        static const SubGhzAnalyzeFreqAttack t[] = {
            {433920000u, SubBruteAttackNICE12bit433}, {868350000u, SubBruteAttackNICE12bit868}};
        return subghz_analyze_nearest(freq, t, 2);
    }
    case SubGhzChipAnsonic: {
        static const SubGhzAnalyzeFreqAttack t[] = {
            {433075000u, SubBruteAttackAnsonic12bit433075},
            {433920000u, SubBruteAttackAnsonic12bit433},
            {434075000u, SubBruteAttackAnsonic12bit434}};
        return subghz_analyze_nearest(freq, t, 3);
    }
    case SubGhzChipHoltek: {
        static const SubGhzAnalyzeFreqAttack t[] = {
            {315000000u, SubBruteAttackHoltek12bitAM315}, {433920000u, SubBruteAttackHoltek12bitAM433},
            {868350000u, SubBruteAttackHoltek12bitAM868}, {915000000u, SubBruteAttackHoltek12bitAM915}};
        return subghz_analyze_nearest(freq, t, 4);
    }
    case SubGhzChipChamberlain: {
        if(bits <= 7) {
            static const SubGhzAnalyzeFreqAttack t[] = {
                {300000000u, SubBruteAttackChamberlain7bit300},
                {315000000u, SubBruteAttackChamberlain7bit315},
                {390000000u, SubBruteAttackChamberlain7bit390}};
            return subghz_analyze_nearest(freq, t, 3);
        } else if(bits == 8) {
            static const SubGhzAnalyzeFreqAttack t[] = {
                {300000000u, SubBruteAttackChamberlain8bit300},
                {315000000u, SubBruteAttackChamberlain8bit315},
                {390000000u, SubBruteAttackChamberlain8bit390}};
            return subghz_analyze_nearest(freq, t, 3);
        }
        static const SubGhzAnalyzeFreqAttack t[] = {
            {300000000u, SubBruteAttackChamberlain9bit300},
            {315000000u, SubBruteAttackChamberlain9bit315},
            {318000000u, SubBruteAttackChamberlain9bit318},
            {390000000u, SubBruteAttackChamberlain9bit390},
            {433920000u, SubBruteAttackChamberlain9bit433}};
        return subghz_analyze_nearest(freq, t, 5);
    }
    case SubGhzChipLinear: {
        static const SubGhzAnalyzeFreqAttack t[] = {
            {300000000u, SubBruteAttackLinear10bit300}, {310000000u, SubBruteAttackLinear10bit310}};
        return subghz_analyze_nearest(freq, t, 2);
    }
    case SubGhzChipLinearDelta:
        return SubBruteAttackLinearDelta8bit310;
    case SubGhzChipPrinceton: {
        // Generic 24-bit PWM -> default to PT2262 variants (most common EV1527/PT2262 receivers).
        static const SubGhzAnalyzeFreqAttack t[] = {
            {315000000u, SubBruteAttackPT226224bit315}, {418000000u, SubBruteAttackPT226224bit418},
            {430000000u, SubBruteAttackPT226224bit430}, {430500000u, SubBruteAttackPT226224bit4305},
            {433920000u, SubBruteAttackPT226224bit433}};
        return subghz_analyze_nearest(freq, t, 5);
    }
    default:
        return -1;
    }
}

// Pre-select the resolved attack and jump into the SubBrute setup screen (the
// user still confirms transmit there). Mirrors subghz_scene_bf_start.c.
static void subghz_analyze_launch_attack(SubGhz* subghz, SubBruteAttacks attack) {
    uint8_t total_repeats = subbrute_settings_get_current_repeats(subghz->subbrute_settings);
    subghz->subbrute_settings->last_index = attack;
    if((subbrute_device_attack_set(subghz->subbrute_device, attack, total_repeats) !=
        SubBruteFileResultOk) ||
       (!subbrute_worker_init_default_attack(
           subghz->subbrute_worker,
           attack,
           subghz->subbrute_device->current_step,
           subghz->subbrute_device->protocol_info,
           subghz->subbrute_device->extra_repeats))) {
        return; // setup failed -> stay on Signal-ID, no navigation
    }
    subbrute_settings_save(subghz->subbrute_settings);
    scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack);
}
#endif // PUBLIC_BUILD

void subghz_scene_analyze_on_enter(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
#ifndef PUBLIC_BUILD
    s_analyze_attack = -1;
#endif

    // Re-load + deserialize the selected capture (self-contained, read-only —
    // mirrors the rolling-analyzer scene).
    bool loaded = subghz_txrx_load_decoder_by_name_protocol(
        subghz->txrx,
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
    FlipperFormat* fff = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    if(loaded && fff) {
        subghz_protocol_decoder_base_deserialize(subghz_txrx_get_decoder(subghz->txrx), fff);
    }

    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);

    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Signal ID");

    if(!decoder || !decoder->protocol || !fff) {
        widget_add_string_element(
            subghz->widget, 0, 16, AlignLeft, AlignTop, FontSecondary, "No capture to analyze.");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    const char* proto_name = decoder->protocol->name;

    SubGhzBlockGeneric generic = {0};
    subghz_block_generic_deserialize(&generic, fff);

    SubGhzRadioPreset* preset =
        subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
    uint32_t freq = preset ? preset->frequency : 0;
    const char* preset_name = (preset && preset->name) ? furi_string_get_cstr(preset->name) : "";

    SignalObservation obs = {
        .freq = freq,
        .mod = subghz_scene_analyze_mod_of(preset_name),
        .bits = (int32_t)generic.data_count_bit,
        .te_short = 0, // not available per-capture; the decoded name is authoritative
        .te_long = 0,
        .name = proto_name,
    };

    SignalMatch matches[3];
    size_t n = signal_fingerprint_classify(&obs, matches, 3);

    FuriString* body = furi_string_alloc();
    const SignalMatch* m = &matches[0];

    if(n == 0 || strcmp(m->name, "UNKNOWN") == 0) {
        furi_string_cat_printf(body, "Proto: %s\n", proto_name ? proto_name : "?");
        furi_string_cat_printf(
            body, "%s  %lu.%02lu MHz  %db\n",
            obs.mod == SIG_MOD_FM ? "FM" : (obs.mod == SIG_MOD_AM ? "AM" : "?"),
            (unsigned long)(freq / 1000000UL), (unsigned long)((freq % 1000000UL) / 10000UL),
            (int)generic.data_count_bit);
        bool chip_shown = false;
#ifndef PUBLIC_BUILD
        // Fixed-code encoder-chip ID + guided-attack setup (PRIVATE only).
        if(!subghz_scene_analyze_is_dynamic(subghz)) {
            SubGhzChipId chip =
                subghz_chip_identify(proto_name, (int32_t)generic.data_count_bit, freq, obs.mod);
            if(chip.family != SubGhzChipUnknown) {
                furi_string_cat_printf(body, "Chip: %s\nFixed-code", chip.chip_label);
                s_analyze_attack =
                    subghz_analyze_chip_to_attack(chip.family, chip.total_bits, freq);
                chip_shown = true;
            }
        } else if(proto_name && strcmp(proto_name, SUBGHZ_PROTOCOL_KEELOQ_NAME) == 0) {
            // Rolling KeeLoq capture: label with the registry family + whatever
            // manufacturer the decoder's own dictionary attack already matched
            // (lib/subghz/protocols/keeloq.c tries every keystore key on
            // deserialize, see keeloq.c:444) -- surfaced here, not re-run.
            char mf[24] = {0};
            if(subghz_scene_analyze_keeloq_mf(decoder, mf, sizeof(mf))) {
                KeeloqTypeInfo info = subghz_scene_analyze_keeloq_family(subghz, mf);
                furi_string_cat_printf(
                    body,
                    "KeeLoq: %s\nMaker: %s (candidate)",
                    subghz_scene_analyze_family_label(info.family),
                    mf);
            } else {
                furi_string_cat_str(body, "KeeLoq rolling code\nMaker key: unknown");
            }
            chip_shown = true;
        }
#endif
        if(!chip_shown) {
            furi_string_cat_str(body, "Not a known automotive fob.\nRaw / generic capture.");
        }
    } else {
        furi_string_cat_printf(body, "ID: %s\n", m->make ? m->make : m->name);
        furi_string_cat_printf(
            body, "Proto: %s  %d%%\n", m->name, (int)m->score);
        furi_string_cat_printf(
            body, "%s  %lu.%02lu MHz  %db\n",
            obs.mod == SIG_MOD_FM ? "FM" : (obs.mod == SIG_MOD_AM ? "AM" : "?"),
            (unsigned long)(freq / 1000000UL), (unsigned long)((freq % 1000000UL) / 10000UL),
            (int)generic.data_count_bit);
        furi_string_cat_printf(
            body, "%s  TX:%s  [%s]",
            m->rolling ? "Rolling" : "Fixed", m->tx ? "yes" : "no",
            subghz_scene_analyze_action_label(m->action));
        if(m->ambiguous) {
            furi_string_cat_str(body, "\nAmbiguous - decode to confirm");
        }
    }

    widget_add_string_multiline_element(
        subghz->widget, 0, 13, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(body));
    furi_string_free(body);

#ifndef PUBLIC_BUILD
    // Spoke: for rolling / PSA captures, offer the deeper tool (rolling-analyzer
    // clone/save or PSA brute) — PRIVATE only (car-clone attack surface).
    if(subghz_scene_analyze_is_dynamic(subghz)) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeLeft,
            subghz_scene_analyze_is_psa(subghz) ? "Brute" : "Details",
            subghz_scene_analyze_button_callback,
            subghz);
    }
#endif

#ifndef PUBLIC_BUILD
    // Spoke: fixed-code with a matching framed brute -> guided Attack (PRIVATE only).
    if(s_analyze_attack >= 0) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Attack",
            subghz_scene_analyze_button_callback,
            subghz);
    } else if(
        subghz_scene_analyze_is_dynamic(subghz) && proto_name &&
        strcmp(proto_name, SUBGHZ_PROTOCOL_KEELOQ_NAME) == 0) {
        // Spoke: rolling KeeLoq capture -> surface the decoder's own dictionary-
        // attack result (candidate maker, or the honest no-match message). Right
        // is free here because the fixed-code Attack above only ever fires for a
        // non-dynamic capture (mutually exclusive with this branch).
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "KeyAtk",
            subghz_scene_analyze_button_callback,
            subghz);
    }
#endif

#ifndef PUBLIC_BUILD
    // Spoke: supported car fob -> the multi-button rolling fob grid (the same
    // emulator VAG/KeeLoq use). On the free Center slot so the rolling analyzer /
    // PSA brute (Left) stays reachable for the same capture. PRIVATE only.
    if(subghz_scene_analyze_can_emulate(subghz)) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Emul",
            subghz_scene_analyze_button_callback,
            subghz);
    }
#endif

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_analyze_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
#ifndef PUBLIC_BUILD
        if(event.event == SubGhzCustomEventSceneAnalyzeDetails) {
            if(subghz_scene_analyze_is_psa(subghz)) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzScenePsaBruteforce);
            } else {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollingAnalyzer);
            }
            return true;
        }
#endif
#ifndef PUBLIC_BUILD
        if(event.event == SubGhzCustomEventSceneAnalyzeAttack) {
            if(s_analyze_attack >= 0) {
                subghz_analyze_launch_attack(subghz, (SubBruteAttacks)s_analyze_attack);
            }
            return true;
        }
#endif
#ifndef PUBLIC_BUILD
        if(event.event == SubGhzCustomEventSceneAnalyzeEmulate) {
            // Supported car fob -> open the multi-button rolling fob grid. Re-load
            // the decoder + preset for this capture (the Signal-ID load path sets
            // the decoder but not the TX preset), copy the captured .sub-format
            // frame into txrx fff_data (what the fob scene + TX path read), then
            // hand off via the generic path.
            if(!subghz_txrx_load_decoder_by_name_protocol(
                   subghz->txrx,
                   subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen))) {
                return false;
            }
            SubGhzRadioPreset* preset =
                subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
            subghz_txrx_set_tx_power(preset->data, preset->data_size, subghz->tx_power);
            subghz_txrx_set_preset(
                subghz->txrx,
                furi_string_get_cstr(preset->name),
                preset->frequency,
                preset->data,
                preset->data_size);
            Stream* dst = flipper_format_get_raw_stream(subghz_txrx_get_fff_data(subghz->txrx));
            Stream* src = flipper_format_get_raw_stream(
                subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen));
            stream_clean(dst);
            stream_rewind(src);
            stream_copy_full(src, dst);
            subghz->keeloq_fob_from_garage = true;
            subghz->keeloq_fob_generic = true;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
            return true;
        }
#endif
#ifndef PUBLIC_BUILD
        if(event.event == SubGhzCustomEventSceneAnalyzeDictAttack) {
            // Surface the decoder's own dictionary-attack result: it already
            // tried every keystore key on deserialize (lib/subghz/protocols/
            // keeloq.c:444) -- this never re-runs an attack, it only reports
            // what the decoder found. Re-load + re-deserialize fresh (the RX
            // hopper may have reassigned the shared decoder since on_enter drew
            // this screen), same pattern as the other spokes above.
            bool loaded = subghz_txrx_load_decoder_by_name_protocol(
                subghz->txrx,
                subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen));
            FlipperFormat* fff =
                subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
            SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
            if(loaded && fff && decoder) {
                subghz_protocol_decoder_base_deserialize(decoder, fff);
            }

            widget_reset(subghz->widget);
            widget_add_string_element(
                subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Dictionary Attack");

            if(!loaded || !fff || !decoder) {
                widget_add_string_multiline_element(
                    subghz->widget,
                    0,
                    16,
                    AlignLeft,
                    AlignTop,
                    FontSecondary,
                    "No capture to analyze.");
                view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
                return true;
            }

            char mf[24] = {0};
            FuriString* body = furi_string_alloc();
            if(subghz_scene_analyze_keeloq_mf(decoder, mf, sizeof(mf))) {
                // ~216 keystore keys against a ~12-bit KeeLoq discriminator ->
                // the coverage matrix measured ~23/179 false-accepts brute-
                // forcing every key. Report this as a candidate, not a verdict.
                KeeloqTypeInfo info = subghz_scene_analyze_keeloq_family(subghz, mf);
                furi_string_cat_printf(
                    body,
                    "Candidate maker: %s\nFamily: %s\n\n"
                    "216-key match has a real\nfalse-accept rate (~12-bit\n"
                    "discriminator) - confirm\nbefore relying on it.",
                    mf,
                    subghz_scene_analyze_family_label(info.family));
            } else {
                furi_string_cat_str(
                    body,
                    "No known key matches.\nExhaustive key recovery is\n"
                    "2^64 - not feasible on-\ndevice. Capture more, or\n"
                    "use the host correlation\ntool.");
            }
            widget_add_string_multiline_element(
                subghz->widget, 0, 16, AlignLeft, AlignTop, FontSecondary,
                furi_string_get_cstr(body));
            furi_string_free(body);

            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
            return true;
        }
#endif
    }
    return false;
}

void subghz_scene_analyze_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
