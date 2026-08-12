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
