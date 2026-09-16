// Host stubs specific to the MAIN/standard-OOK harness (stress_main.c). Kept
// SEPARATE from host_shim_extra.c so the car (build_full) harness is untouched.
// Only missing HAL/environment symbols are stubbed here -- never decoder logic.
//
// nice_flor_s / alutech_at_4n ask the environment for an optional rainbow-table
// file name; on host there is no filesystem, so return NULL (the decoders
// null-check it and fall back to their built-in tables). Returning NULL never
// changes the decode path this harness exercises.
#include <stddef.h>

typedef struct SubGhzEnvironment SubGhzEnvironment;

const char* subghz_environment_get_nice_flor_s_rainbow_table_file_name(SubGhzEnvironment* e) {
    (void)e;
    return NULL;
}
const char* subghz_environment_get_alutech_at_4n_rainbow_table_file_name(SubGhzEnvironment* e) {
    (void)e;
    return NULL;
}
