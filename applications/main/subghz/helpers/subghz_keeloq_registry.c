#include "subghz_keeloq_registry.h"
#include <string.h>

// Learning-type numbers are the keystore's on-disk `type` field (a wire value),
// not KeeLoq crypto, so we switch on literals (symbolic names in comments, per
// Global Constraints) and stay furi-free / host-testable. Values:
// 0 UNKNOWN,1 SIMPLE,2 NORMAL,3 SECURE,4 MAGIC_XOR_1,5 FAAC,6 MAGIC_SERIAL_1,
// 7 MAGIC_SERIAL_2,8 MAGIC_SERIAL_3,10 SIMPLE_KINGGATES,11 NORMAL_JAROLIFT,
// 12 ERREKA,13 PUJOL,14 AERF,15 SIMPLE_JCM (9 is undefined in the enum).

static bool name_is_kia(const char* name) {
    return name && (strncmp(name, "KIA", 3) == 0);
}

static bool name_has_beninca(const char* name) {
    return name && (strstr(name, "Beninca") != NULL);
}

KeeloqTypeInfo subghz_keeloq_registry_type_info(uint16_t type, const char* name) {
    KeeloqTypeInfo r = {KlFamGeneric, GenKeeloq, false, false};
    switch(type) {
    case 1:  // SIMPLE
    case 2:  // NORMAL
    case 3:  // SECURE
    case 4:  // MAGIC_XOR_TYPE_1
    case 6:  // MAGIC_SERIAL_TYPE_1
    case 7:  // MAGIC_SERIAL_TYPE_2
    case 8:  // MAGIC_SERIAL_TYPE_3
    case 12: // ERREKA
    case 13: // PUJOL
    case 14: // AERF
    case 15: // SIMPLE_JCM
        r.family = KlFamGeneric; r.generator = GenKeeloq; r.encodable = true;
        return r;
    case 0:  // UNKNOWN — generic dictionary key, v1 decode-only
        r.family = KlFamGeneric; r.decode_only = true; return r;
    case 5:  // FAAC
        r.family = KlFamFaacSLH; r.generator = GenFaacSLH; r.encodable = true; return r;
    case 10: // SIMPLE_KINGGATES (or KIA/Beninca by name)
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        // Beninca under type 10: NOT encodable. beninca_arc.c's key lookup
        // (get_subghz_protocol_beninca_arc_aes_key) only ever matches a
        // keystore entry with type==9 (BENINCA_ARC_KEY_TYPE), regardless of
        // name — a type-10 Beninca entry routed to GenBenincaARC would
        // silently generate with whatever type-9 key happens to be present,
        // not its own. Decode-only until the protocol itself matches by name.
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.decode_only = true; return r; }
        r.family = KlFamKingGates; r.generator = GenKingGatesStylo4k; r.encodable = true; return r;
    case 11: // NORMAL_JAROLIFT (or KIA/Beninca by name)
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        // Beninca under type 11: NOT encodable, same reason as type 10 above
        // (the AES-key lookup is type==9-only).
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.decode_only = true; return r; }
        r.family = KlFamJarolift; r.generator = GenJarolift; r.encodable = true; return r;
    case 9:  // undefined enum slot; KIA_Hyundai_V5 (car), Beninca_ARC (GenBenincaARC), else orphan
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.generator = GenBenincaARC; r.encodable = true; return r; }
        r.family = KlFamOrphan; r.decode_only = true; return r;
    default:
        r.family = KlFamOrphan; r.decode_only = true; return r;
    }
}

bool subghz_keeloq_registry_type_encodable(uint16_t type) {
    return subghz_keeloq_registry_type_info(type, NULL).encodable;
}
