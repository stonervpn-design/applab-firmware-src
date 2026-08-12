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
    assert(subghz_keeloq_registry_type_info(11, "Jarolift").family == KlFamJarolift);

    // Name disambiguation for shared types
    assert(subghz_keeloq_registry_type_info(10, "KIA_Hyundai_V3&4").family == KlFamKiaCar);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").family == KlFamKiaCar);
    // Beninca appears under types 9/11 but has a real GenBenincaARC route
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").family == KlFamBeninca);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").encodable);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").generator == GenBenincaARC);
    // Kia is decode-only (capture/car path), not catalog-encodable
    assert(!subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").encodable);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").decode_only);

    printf("test_keeloq_registry: ALL PASS\n");
    return 0;
}
