#include "subghz_fob_buttons.h"

#include <string.h>

/* ============================================================================
 * CAR-FOB PROTOCOL AUDIT  (lib/subghz/protocols, HEAD feat/keeloq-no-desync-clone)
 *
 * For each car keyfob protocol: does its encoder deserialize read an INDEPENDENT
 * button field ("Btn"/"Button"/"BtnLo") and rebuild the transmitted frame from
 * it (so writing a new button to the .sub changes the on-air button)? And does
 * it expose an enumerable button code->name map?
 *
 * SUPPORTED     = independent button read + enumerable name map -> in the grid.
 * SYNTH-NO-MAP  = independent button read but NO name map (free byte / KeeLoq
 *                 nibble) -> TX-capable but not enumerable, NOT in grid.
 * REPLAY        = encoder replays the captured Key/rolling code, or the button
 *                 is driven only by the custom_btn UI override (not a .sub
 *                 field) -> plain transmitter, NOT in grid.
 * UNSUPPORTED   = no functional encoder (NULL/stub / no Send flag).
 *
 *  Protocol name     | class        | grid buttons (codes)
 *  ------------------+--------------+----------------------------------------
 *  "VAG"             | SUPPORTED    | 3  0x1/0x2/0x4  (existing view layout)
 *  "KeeLoq"          | SUPPORTED    | 4  0x1/0x2/0x4/0x8 (existing view layout)
 *  "Kia V0"          | SUPPORTED    | 3  0x1/0x2/0x3  (base Kia map)
 *  "Kia V1"          | SUPPORTED    | 3  0x1/0x2/0x3
 *  "Kia V2"          | SYNTH-NO-MAP | -  Btn rebuilds frame, no name map
 *  "Kia V3/V4"       | SYNTH-NO-MAP | -  KeeLoq re-encrypt, no name map
 *  "Kia V5"          | SYNTH-NO-MAP | -  mixer re-encrypt, no name map
 *  "Kia V6"          | SYNTH-NO-MAP | -  AES re-encrypt, no name map
 *  "Kia V7"          | SUPPORTED    | 4  0x1/0x2/0x3/0x8
 *  "Hyundai/Kia RIO" | SUPPORTED    | 4  0x1/0x2/0x4/0x8
 *  "Hyundai TL"      | REPLAY       | -  rolling, verbatim payload
 *  "SantaFe 13-16"   | SUPPORTED    | 4  0x1/0x2/0x4/0x8
 *  "Ford V0"         | SUPPORTED    | 4  0x1/0x2/0x4/0x8
 *  "Ford V1"         | SUPPORTED    | 4  0x1/0x2/0x4/0x8 (0x0 Sync omitted)
 *  "Ford V2"         | REPLAY       | -  button baked into Key
 *  "Ford V3"         | SUPPORTED    | 2  0x1/0x2
 *  "Honda Static"    | SUPPORTED    | 6  0x1/0x2/0x4/0x5/0x8/0x9
 *  "HondaV2&V3"      | SUPPORTED    | 3  0x20/0x40/0x60 (V2 path; V3=replay)
 *  "HondaV4"         | REPLAY       | -  custom_btn override, no Btn field
 *  "HondaV5"         | SUPPORTED    | 2  0x21/0x22   (field "Button")
 *  "HondaV6"         | REPLAY       | -  Key replay
 *  "HondaV7"         | SUPPORTED    | 6  0x0/0x1/0x2/0x4/0x8/0x9 (field "Button")
 *  "HondaV8"         | SUPPORTED    | 6  0x0/0x1/0x2/0x4/0x8/0x9 (field "Button")
 *  "HondaV9"         | SUPPORTED    | 6  0x0/0x1/0x2/0x4/0x8/0x9 (field "Button")
 *  "HondaV10"        | REPLAY       | -  raw Key replay
 *  "Mazda V0"        | SUPPORTED    | 4  0x1/0x2/0x4/0x8
 *  "Mazda V1"        | REPLAY       | -  button baked into Key
 *  "Mazda V2"        | REPLAY       | -  custom_btn override
 *  "MazdaSiemens"    | SYNTH-NO-MAP | -  free byte, no name map
 *  "Subaru"          | SUPPORTED    | 4  0x0/0x1/0x2/0x3
 *  "Fiat V1"         | SUPPORTED    | 4  0x1/0x2/0x4/0x8 (needs Hitag2 Key)
 *  "FIAT SPA"        | REPLAY       | -
 *  "Fiat V0"         | REPLAY       | -
 *  "Fiat Multi"      | REPLAY       | -  custom_btn override
 *  "Ducato V0"       | REPLAY       | -  custom_btn override
 *  "Chrysler V0"     | SUPPORTED    | 2  0x1/0x2
 *  "Jeep V0"         | REPLAY       | -  16-bit codes baked into Key
 *  "Peugot V0"       | REPLAY       | -
 *  "Opel V0"         | REPLAY       | -
 *  "LDV V0"          | UNSUPPORTED  | -  stub encoder, no Send flag
 *  "Land Rover V0"   | SUPPORTED    | 2  0x2/0x4
 *  "ChevroletV0"     | REPLAY       | -
 *  "Nissan V0"       | REPLAY       | -
 *  "Toyota V0"       | REPLAY       | -
 *  "Lexus V0"        | REPLAY       | -
 *  "Audi V1"         | REPLAY       | -  custom_btn override
 *  "Audi V2"         | REPLAY       | -
 *  "Audi V3"         | REPLAY       | -  button in Key
 *  "BMW_v2"          | REPLAY       | -
 *  "BMW_v3"          | REPLAY       | -  button in Key
 *  "BMW CAS4"        | UNSUPPORTED  | -  NULL encoder (decode-only)
 *  "Renault V1".."V4"| REPLAY       | -  encrypted rolling replay
 *  "Mitsubishi V0"   | SYNTH-NO-MAP | -  Btn rebuilds frame, no name map
 *  "Mitsubishi V1"   | REPLAY       | -  button in Key
 *  "Porsche Touareg" | SYNTH-NO-MAP | -  Btn rebuilds frame, no name map
 *  "Sheriff CFM"     | SYNTH-NO-MAP | -  codes 0x10/0x20/0x40/0x80, no names
 *  "Nord ICE"        | REPLAY       | -  button in Key
 *  "Star Line"       | SYNTH-NO-MAP | -  KeeLoq synth, no name map
 *  "Scher-Khan"      | REPLAY       | -  verbatim bitstream, Btn cosmetic
 *
 * All button codes/names below are transcribed verbatim from each protocol's
 * own button-name map (honda_static_button_names, kia_v0_button_name, the
 * ford_v0 get_string map, vag_button_name, etc.) - none invented. Codes are the
 * values written to that protocol's button field.
 *
 * NOTE for the TX path (Task 3): the field name differs by family - most read
 * "Btn"; Honda V5/V7/V8/V9 read "Button"; Honda V2&V3 reads "BtnLo". The write
 * path must use the matching field name per protocol.
 * ==========================================================================*/

// --- VAG (aut64): vag_button_name 0x1/0x2/0x4 (existing 3-tile view layout) ---
static const SubGhzFobButton FOB_VAG[] = {
    {0x1, "UNLOCK"},
    {0x2, "LOCK"},
    {0x4, "BOOT"},
};

// --- Generic KeeLoq: existing 4-tile view layout (nibble codes 0x1/0x2/0x4/0x8) ---
static const SubGhzFobButton FOB_KEELOQ[] = {
    {0x1, "LOCK"},
    {0x2, "UNLOCK"},
    {0x4, "TRUNK"},
    {0x8, "PANIC"},
};

#ifndef PUBLIC_BUILD
// --- Kia V0: kia_v0_button_name base (Kia) map ---
static const SubGhzFobButton FOB_KIA_V0[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x3, "Trunk"},
};

// --- Kia V1: kia_v1 button_name ---
static const SubGhzFobButton FOB_KIA_V1[] = {
    {0x1, "Close"},
    {0x2, "Open"},
    {0x3, "Boot"},
};

// --- Kia V7: kia_v7 button_name (0x8 also maps to Trunk) ---
static const SubGhzFobButton FOB_KIA_V7[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x3, "Trunk"},
    {0x8, "Trunk"},
};

// --- Hyundai/Kia RIO: hkr_button_to_mask/hkr_button_name (Btn 1/2/4/8) ---
static const SubGhzFobButton FOB_HYUNDAI_KIA_RIO[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x4, "Trunk"},
    {0x8, "Panic"},
};

// --- SantaFe 13-16: sf_button_name ---
static const SubGhzFobButton FOB_SANTA_FE[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x4, "Trunk"},
    {0x8, "Panic"},
};

// --- Ford V0: get_string inline map ---
static const SubGhzFobButton FOB_FORD_V0[] = {
    {0x1, "Panic"},
    {0x2, "Lock"},
    {0x4, "Unlock"},
    {0x8, "Boot"},
};

// --- Ford V1: ford_v1 button map (0x0 Sync omitted - not a fob action) ---
static const SubGhzFobButton FOB_FORD_V1[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x4, "Trunk"},
    {0x8, "Panic"},
};

// --- Ford V3: FORD_V3_BTN_LOCK/UNLOCK (encoder only re-bakes these two) ---
static const SubGhzFobButton FOB_FORD_V3[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
};

// --- Honda Static: honda_static_button_names, only 0x336-mask-valid buttons ---
static const SubGhzFobButton FOB_HONDA_STATIC[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x4, "Trunk"},
    {0x5, "Remote Start"},
    {0x8, "Panic"},
    {0x9, "Lock x2"},
};

// --- Honda V2&V3: V2 map (btn_byte2). V3 path is Key-baked replay. Field "BtnLo". ---
static const SubGhzFobButton FOB_HONDA_V2_V3[] = {
    {0x20, "LOCK"},
    {0x40, "UNLOCK"},
    {0x60, "TRUNK"},
};

// --- Honda V5: honda_v5 button map. Field "Button". ---
static const SubGhzFobButton FOB_HONDA_V5[] = {
    {0x21, "LOCK"},
    {0x22, "UNLOCK"},
};

// --- Honda V7/V8/V9 share one button map (OOK/KeeLoq/FSK). Field "Button". ---
static const SubGhzFobButton FOB_HONDA_V789[] = {
    {0x0, "Panic"},
    {0x1, "Lock"},
    {0x2, "Trunk"},
    {0x4, "Remote Start"},
    {0x8, "Unlock"},
    {0x9, "Unlock x2"},
};

// --- Mazda V0: mazda_v0 button map ---
static const SubGhzFobButton FOB_MAZDA_V0[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
    {0x4, "Trunk"},
    {0x8, "Remote"},
};

// --- Subaru: subaru map indexed by (btn & 0x03) ---
static const SubGhzFobButton FOB_SUBARU[] = {
    {0x0, "Lock"},
    {0x1, "Unlock"},
    {0x2, "Trunk"},
    {0x3, "Panic"},
};

// --- Fiat V1: fiat_v1 button map (encoder needs a Hitag2 Key field present) ---
static const SubGhzFobButton FOB_FIAT_V1[] = {
    {0x1, "Close"},
    {0x2, "Trunk"},
    {0x4, "Lock"},
    {0x8, "Unlock"},
};

// --- Chrysler V0: chrysler_v0 button map (encoder honours 0x1/0x2 only) ---
static const SubGhzFobButton FOB_CHRYSLER_V0[] = {
    {0x1, "Lock"},
    {0x2, "Unlock"},
};

// --- Land Rover V0: land_rover_v0 signature map (0x0 Unknown omitted) ---
static const SubGhzFobButton FOB_LAND_ROVER_V0[] = {
    {0x2, "Lock"},
    {0x4, "Unlock"},
};
#endif // PUBLIC_BUILD

typedef struct {
    const char* protocol_name;
    const SubGhzFobButton* buttons;
    size_t count;
} SubGhzFobEntry;

#define FOB_ENTRY(name, table) \
    { (name), (table), sizeof(table) / sizeof((table)[0]) }

static const SubGhzFobEntry subghz_fob_table[] = {
    FOB_ENTRY("VAG", FOB_VAG),
    FOB_ENTRY("KeeLoq", FOB_KEELOQ),
#ifndef PUBLIC_BUILD
    FOB_ENTRY("Kia V0", FOB_KIA_V0),
    FOB_ENTRY("Kia V1", FOB_KIA_V1),
    FOB_ENTRY("Kia V7", FOB_KIA_V7),
    FOB_ENTRY("Hyundai/Kia RIO", FOB_HYUNDAI_KIA_RIO),
    FOB_ENTRY("SantaFe 13-16", FOB_SANTA_FE),
    FOB_ENTRY("Ford V0", FOB_FORD_V0),
    FOB_ENTRY("Ford V1", FOB_FORD_V1),
    FOB_ENTRY("Ford V3", FOB_FORD_V3),
    FOB_ENTRY("Honda Static", FOB_HONDA_STATIC),
    FOB_ENTRY("HondaV2&V3", FOB_HONDA_V2_V3),
    FOB_ENTRY("HondaV5", FOB_HONDA_V5),
    FOB_ENTRY("HondaV7", FOB_HONDA_V789),
    FOB_ENTRY("HondaV8", FOB_HONDA_V789),
    FOB_ENTRY("HondaV9", FOB_HONDA_V789),
    FOB_ENTRY("Mazda V0", FOB_MAZDA_V0),
    FOB_ENTRY("Subaru", FOB_SUBARU),
    FOB_ENTRY("Fiat V1", FOB_FIAT_V1),
    FOB_ENTRY("Chrysler V0", FOB_CHRYSLER_V0),
    FOB_ENTRY("Land Rover V0", FOB_LAND_ROVER_V0),
#endif // PUBLIC_BUILD
};

const SubGhzFobButton* subghz_fob_buttons(const char* protocol_name, size_t* out_n) {
    if(out_n) *out_n = 0;
    if(!protocol_name) return NULL;

    for(size_t i = 0; i < sizeof(subghz_fob_table) / sizeof(subghz_fob_table[0]); i++) {
        if(strcmp(protocol_name, subghz_fob_table[i].protocol_name) == 0) {
            if(out_n) *out_n = subghz_fob_table[i].count;
            return subghz_fob_table[i].buttons;
        }
    }
    return NULL;
}

bool subghz_fob_protocol_supported(const char* protocol_name) {
    size_t n = 0;
    return subghz_fob_buttons(protocol_name, &n) != NULL && n > 0;
}

bool subghz_fob_single_button_synth(const char* protocol_name) {
#ifndef PUBLIC_BUILD
    if(!protocol_name) return false;
    return strcmp(protocol_name, "Kia V5") == 0 || strcmp(protocol_name, "PSA") == 0 ||
           strcmp(protocol_name, "Fiat V0") == 0;
#else
    // PRIVATE-only: the car protocol name literals must not ship in a public
    // build (verify_public_no_car). No single-button-synth protocol is public.
    (void)protocol_name;
    return false;
#endif
}

SubGhzFobEmulateKind subghz_fob_emulate_kind(const char* protocol_name) {
    if(!protocol_name) return SubGhzFobEmulateReplay;
    // VAG and KeeLoq are single-button rolling clones handled by dedicated paths
    // in subghz_scene_keeloq_fob; every other rolling second fob routes through
    // the multi-button counter-rolling path iff it has an enumerable button table,
    // or the single-button "resend rolling" path for synth-no-table protocols.
    if(strcmp(protocol_name, "VAG") == 0 || strcmp(protocol_name, "KeeLoq") == 0) {
        return SubGhzFobEmulateSynth;
    }
    if(subghz_fob_protocol_supported(protocol_name) ||
       subghz_fob_single_button_synth(protocol_name)) {
        return SubGhzFobEmulateSynth;
    }
    return SubGhzFobEmulateReplay;
}

const char* subghz_fob_emulate_kind_str(SubGhzFobEmulateKind kind) {
    return kind == SubGhzFobEmulateSynth ? "Second fob" : "Replay";
}

// Protocols whose encoder reads the button from a field other than "Btn". The
// Honda OOK/KeeLoq/FSK families (V5/V7/V8/V9) read "Button"; the combined
// V2&V3 protocol reads "BtnLo". Everything else uses the "Btn" default.
const char* subghz_fob_button_field(const char* protocol_name) {
#ifndef PUBLIC_BUILD
    if(protocol_name) {
        if(strcmp(protocol_name, "HondaV5") == 0 || strcmp(protocol_name, "HondaV7") == 0 ||
           strcmp(protocol_name, "HondaV8") == 0 || strcmp(protocol_name, "HondaV9") == 0) {
            return "Button";
        }
        if(strcmp(protocol_name, "HondaV2&V3") == 0) {
            return "BtnLo";
        }
    }
#else
    (void)protocol_name;
#endif // PUBLIC_BUILD
    return "Btn";
}

// The counter field each protocol's ENCODER deserialize reads. Verified against
// each encoder in lib/subghz/protocols: santa_fe/honda_v2_v3 read "Rolling",
// honda_v7/honda_v9 read "Counter", honda_v8 reads "RollingCode", honda_v5 reads
// "Counter". Every other supported protocol keeps the counter in FF_CNT ("Cnt")
// via pp_encoder_read_fields. The generic fob press path advances the returned
// field, not a hardcoded "Cnt", so the on-air rolling code actually changes.
const char* subghz_fob_counter_field(const char* protocol_name) {
#ifndef PUBLIC_BUILD
    if(protocol_name) {
        if(strcmp(protocol_name, "SantaFe 13-16") == 0 ||
           strcmp(protocol_name, "HondaV2&V3") == 0) {
            return "Rolling";
        }
        if(strcmp(protocol_name, "HondaV8") == 0) {
            return "RollingCode";
        }
        if(strcmp(protocol_name, "HondaV5") == 0 || strcmp(protocol_name, "HondaV7") == 0 ||
           strcmp(protocol_name, "HondaV9") == 0) {
            return "Counter";
        }
    }
#else
    (void)protocol_name;
#endif // PUBLIC_BUILD
    return "Cnt";
}
