#include "subghz_chip_id.h"

#ifndef PUBLIC_BUILD

#include <string.h>

// Encoder-chip classifier (pure). PRECONDITION: call only for FIXED-code
// captures. The analyze scene gates on protocol type (dynamic -> rolling tools)
// before invoking this, so substring matching on the decoded name is safe here
// even though some families (Nice/CAME) also have rolling variants. A wrong
// result is a mislabel, never an RF action.

static bool chip_has(const char* haystack, const char* needle) {
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

SubGhzChipId subghz_chip_identify(
    const char* proto_name,
    int32_t bits,
    uint32_t freq_hz,
    SignalMod mod) {
    (void)freq_hz;
    (void)mod;
    SubGhzChipId r = {"", SubGhzChipUnknown, bits, false};

    if(chip_has(proto_name, "Princeton")) {
        r.family = SubGhzChipPrinceton;
        r.chip_label = "Princeton (PT2262/EV1527)";
        r.fixed = true;
    } else if(chip_has(proto_name, "CAME")) {
        r.family = SubGhzChipCame;
        r.chip_label = "CAME";
        r.fixed = true;
    } else if(chip_has(proto_name, "Nice")) {
        r.family = SubGhzChipNice;
        r.chip_label = "NICE FLO";
        r.fixed = true;
    } else if(chip_has(proto_name, "Holtek")) {
        r.family = SubGhzChipHoltek;
        r.chip_label = "Holtek HT12/HT6";
        r.fixed = true;
    } else if(chip_has(proto_name, "Ansonic")) {
        r.family = SubGhzChipAnsonic;
        r.chip_label = "Ansonic";
        r.fixed = true;
    } else if(chip_has(proto_name, "Chamberlain")) {
        r.family = SubGhzChipChamberlain;
        r.chip_label = "Chamberlain";
        r.fixed = true;
    } else if(chip_has(proto_name, "Linear") && chip_has(proto_name, "Delta")) {
        r.family = SubGhzChipLinearDelta;
        r.chip_label = "Linear Delta";
        r.fixed = true;
    } else if(chip_has(proto_name, "Linear")) {
        r.family = SubGhzChipLinear;
        r.chip_label = "Linear";
        r.fixed = true;
    }
    return r;
}

#endif // PUBLIC_BUILD
