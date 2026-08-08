#pragma once
// Resumable brute sessions (PRIVATE-only): persist a long fixed-code SubBrute
// sweep so it survives power-off and resumes from the last checkpoint. Honest
// framing: resume is "from the last checkpoint", not literal last-code-on-yank.
// See docs/superpowers/specs/2026-08-08-resumable-brute-design.md.
#ifndef PUBLIC_BUILD

#include <stdint.h>
#include <stdbool.h>

#define SUBBRUTE_RESUME_SCHEMA 1u

// Mirrors of the SubBruteAttacks enum (helpers/subbrute_protocols.h), duplicated
// here so the pure validator has NO furi dependency and stays host-testable. The
// real build (subbrute_resume_io.c) _Static_asserts these against the actual enum
// so a reorder cannot silently drift them.
#define SUBBRUTE_RESUME_ATTACK_TOTAL    45u // SubBruteAttackTotalCount
#define SUBBRUTE_RESUME_ATTACK_DEBRUIJN 43u // SubBruteAttackDeBruijn (single burst)
#define SUBBRUTE_RESUME_ATTACK_LOADFILE 44u // SubBruteAttackLoadFile (deferred, phase 2)

typedef struct {
    uint32_t schema_version;
    uint32_t attack; // SubBruteAttacks enum index
    uint64_t current_step; // sweep position
    uint8_t extra_repeats;
    uint8_t two_bytes; // bool; affects the keyspace via calc_max_value
    uint8_t opencode;
} SubBruteResume;

// Pure (no furi): a record is resumable iff the schema matches, the attack index
// is in range, and it is a resumable KIND — not DeBruijn (single continuous
// burst, no step) and not LoadFile (deferred to phase 2).
bool subbrute_resume_is_valid(const SubBruteResume* r);

// FlipperFormat glue (subbrute_resume_io.c). All use the canonical resume file
// (APP_DATA_PATH) internally and are gated on storage_sd_status()==FSE_OK; save
// returns false (with a UI-surfaceable failure) when storage is unavailable.
bool subbrute_resume_save(const SubBruteResume* r);
bool subbrute_resume_load(SubBruteResume* out); // true only if present AND is_valid
bool subbrute_resume_has(void); // cheap "offer a Resume entry?" probe
void subbrute_resume_clear(void); // delete on sweep completion

#endif // PUBLIC_BUILD
