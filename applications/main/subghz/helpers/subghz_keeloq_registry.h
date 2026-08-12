#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "subghz_gen_type.h" // GenType (furi-free leaf; NOT subghz_gen_info.h)

typedef enum {
    KlFamGeneric,
    KlFamFaacSLH,
    KlFamKingGates,
    KlFamJarolift,
    KlFamBeninca,
    KlFamKiaCar,
    KlFamOrphan,
} KeeloqFamily;

typedef struct {
    KeeloqFamily family;
    GenType generator; // valid only when encodable
    bool encodable;    // has a working generate/emulate route from the catalog
    bool decode_only;  // reachable by decode/attack but not catalog-generate (v1)
} KeeloqTypeInfo;

// Classify a keystore entry by (learning type, manufacturer name). `name` may be
// NULL when only the type matters; name is consulted only for the shared
// types 9/10/11 (Kia vs dedicated protocol). Never returns an invalid generator
// when encodable is true.
KeeloqTypeInfo subghz_keeloq_registry_type_info(uint16_t type, const char* name);

// Convenience gate used by the by-manufacturer picker (replaces its local list).
bool subghz_keeloq_registry_type_encodable(uint16_t type);

// Name-blind check: true ONLY for the generic-KeeLoq encodable learning types
// (1,2,3,4,6,7,8,12,13,14,15). False for everything else, including the
// shared types (5,9,10,11) that require a name to disambiguate a dedicated
// family (FAAC/KingGates/Jarolift/Beninca/Kia) — those must never be routed
// through the generic GenKeeloq encoder. Pure integer check: no strcmp, no
// call into subghz_keeloq_registry_type_info. Used by the by-manufacturer
// picker, which always generates via GenKeeloq regardless of listed type.
bool subghz_keeloq_registry_type_is_generic_encodable(uint16_t type);
