#include "../subghz_i.h"

#include "../helpers/subghz_txrx_i.h"
#include "../helpers/signal_fingerprint.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/psa.h>
#include <string.h>

#ifndef PUBLIC_BUILD
#include "../helpers/subghz_chip_id.h"
#include "../helpers/subbrute_protocols.h" // SubBruteAttacks enum for the guided-attack handoff
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
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneAnalyzeAttack);
    }
#endif
}

#ifndef PUBLIC_BUILD
// Guided-attack handoff (PRIVATE). Resolved SubBrute attack for the current
// fixed-code capture, or -1. Set in on_enter, consumed in on_event.
static int s_analyze_attack = -1;

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

    // Spoke: for rolling / PSA captures, offer the existing deeper tool.
    if(subghz_scene_analyze_is_dynamic(subghz)) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeLeft,
            subghz_scene_analyze_is_psa(subghz) ? "Brute" : "Details",
            subghz_scene_analyze_button_callback,
            subghz);
    }

#ifndef PUBLIC_BUILD
    // Spoke: fixed-code with a matching framed brute -> guided Attack (PRIVATE only).
    if(s_analyze_attack >= 0) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Attack",
            subghz_scene_analyze_button_callback,
            subghz);
    }
#endif

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_analyze_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneAnalyzeDetails) {
            if(subghz_scene_analyze_is_psa(subghz)) {
                scene_manager_next_scene(subghz->scene_manager, SubGhzScenePsaBruteforce);
            } else {
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollingAnalyzer);
            }
            return true;
        }
#ifndef PUBLIC_BUILD
        if(event.event == SubGhzCustomEventSceneAnalyzeAttack) {
            if(s_analyze_attack >= 0) {
                subghz_analyze_launch_attack(subghz, (SubBruteAttacks)s_analyze_attack);
            }
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
