#pragma once
// GenType lives here (leaf, no includes) so pure/host-testable helpers can use it
// without dragging in subghz_types.h -> <furi.h>. subghz_gen_info.h includes this.
typedef enum {
    GenData, GenFaacSLH, GenKeeloq, GenCameAtomo, GenKeeloqBFT, GenAlutechAt4n,
    GenSomfyTelis, GenSomfyKeytis, GenKingGatesStylo4k, GenBenincaARC, GenJarolift,
    GenDitecGOL4, GenNiceFlorS, GenSecPlus1, GenSecPlus2, GenPhoenixV2,
} GenType;
