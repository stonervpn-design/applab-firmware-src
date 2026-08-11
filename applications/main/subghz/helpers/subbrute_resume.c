// Pure validator for resumable-brute records. Kept furi-free (only the header)
// so it host-tests without the FlipperFormat/subbrute pipeline; the storage I/O
// lives in subbrute_resume_io.c. See the design spec.
#include "subbrute_resume.h"

#ifndef PUBLIC_BUILD

bool subbrute_resume_is_valid(const SubBruteResume* r) {
    if(!r) return false;
    if(r->schema_version != SUBBRUTE_RESUME_SCHEMA) return false;
    if(r->attack >= SUBBRUTE_RESUME_ATTACK_TOTAL) return false;
    if(r->attack == SUBBRUTE_RESUME_ATTACK_DEBRUIJN) return false; // single burst, no step
    if(r->attack == SUBBRUTE_RESUME_ATTACK_LOADFILE) {
        // LoadFile resumes by reloading the source .sub; without a path there is
        // nothing to rebuild the protocol/frequency/preset from.
        if(r->file_path[0] == '\0') return false;
    }
    return true;
}

#endif // PUBLIC_BUILD
