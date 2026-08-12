#include "../applications/main/subghz/helpers/subghz_keeloq_registry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    // Generic encodable types
    uint16_t generic[] = {1,2,3,4,6,7,8,12,13,14,15};
    for(unsigned i = 0; i < sizeof(generic)/sizeof(generic[0]); i++) {
        KeeloqTypeInfo t = subghz_keeloq_registry_type_info(generic[i], NULL);
        assert(t.family == KlFamGeneric);
        assert(t.encodable && !t.decode_only);
        assert(t.generator == GenKeeloq);
    }
    // The four formerly-HIDDEN types are now encodable (the +36-key bug)
    uint16_t hidden[] = {12,13,14,15};
    for(unsigned i = 0; i < 4; i++)
        assert(subghz_keeloq_registry_type_encodable(hidden[i]) == true);

    // type-0 generics: decode-only in v1
    KeeloqTypeInfo t0 = subghz_keeloq_registry_type_info(0, NULL);
    assert(!t0.encodable && t0.decode_only && t0.family == KlFamGeneric);

    // Dedicated protocols
    assert(subghz_keeloq_registry_type_info(5, "FAAC_SLH").family == KlFamFaacSLH);
    assert(subghz_keeloq_registry_type_info(5, "FAAC_SLH").generator == GenFaacSLH);
    assert(subghz_keeloq_registry_type_info(10, "Kingates_Stylo4k").family == KlFamKingGates);
    assert(subghz_keeloq_registry_type_info(10, "Kingates_Stylo4k").generator == GenKingGatesStylo4k);
    assert(subghz_keeloq_registry_type_info(10, "Kingates_Stylo4k").encodable);
    assert(subghz_keeloq_registry_type_info(11, "Jarolift").family == KlFamJarolift);
    assert(subghz_keeloq_registry_type_info(11, "Jarolift").generator == GenJarolift);
    assert(subghz_keeloq_registry_type_info(11, "Jarolift").encodable);

    // Name disambiguation for shared types
    assert(subghz_keeloq_registry_type_info(10, "KIA_Hyundai_V3&4").family == KlFamKiaCar);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").family == KlFamKiaCar);
    // type-11 KIA
    assert(subghz_keeloq_registry_type_info(11, "KIAV6A").family == KlFamKiaCar);
    assert(subghz_keeloq_registry_type_info(11, "KIAV6A").decode_only);
    assert(!subghz_keeloq_registry_type_info(11, "KIAV6A").encodable);
    // Beninca appears under types 9/10/11, but beninca_arc.c's key lookup
    // (get_subghz_protocol_beninca_arc_aes_key) only ever matches a keystore
    // entry with type==9, regardless of name — so only type-9 Beninca has a
    // real GenBenincaARC route; type-10/11 Beninca are decode-only.
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").family == KlFamBeninca);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").encodable);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").generator == GenBenincaARC);
    // type-10 Beninca: decode-only (protocol's key lookup is type==9-only)
    assert(subghz_keeloq_registry_type_info(10, "Beninca_ARC").family == KlFamBeninca);
    assert(subghz_keeloq_registry_type_info(10, "Beninca_ARC").decode_only);
    assert(!subghz_keeloq_registry_type_info(10, "Beninca_ARC").encodable);
    // type-11 Beninca: decode-only (same reason)
    assert(subghz_keeloq_registry_type_info(11, "BenincaVA").family == KlFamBeninca);
    assert(subghz_keeloq_registry_type_info(11, "BenincaVA").decode_only);
    assert(!subghz_keeloq_registry_type_info(11, "BenincaVA").encodable);
    // Kia is decode-only (capture/car path), not catalog-encodable
    assert(!subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").encodable);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").decode_only);

    // type-9 orphan: neither KIA nor Beninca by name (and NULL name)
    KeeloqTypeInfo orphan_named = subghz_keeloq_registry_type_info(9, "SomeUnknownName");
    assert(orphan_named.family == KlFamOrphan);
    assert(orphan_named.decode_only);
    assert(!orphan_named.encodable);
    KeeloqTypeInfo orphan_null = subghz_keeloq_registry_type_info(9, NULL);
    assert(orphan_null.family == KlFamOrphan);
    assert(orphan_null.decode_only);
    assert(!orphan_null.encodable);

    printf("test_keeloq_registry: ALL PASS\n");
    return 0;
}
