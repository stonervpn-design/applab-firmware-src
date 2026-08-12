// KeeLoq master-key console -- Task 2: host coverage matrix.
//
// Proves, for every entry in the host keystore (keystore_host.c), that Task
// 1's registry classification (subghz_keeloq_registry_type_info) matches
// what the REAL lib/subghz/protocols/keeloq.c encoder+decoder actually do:
// every `encodable && KlFamGeneric` entry must genuinely round-trip through
// the real crypto (not just be labelled encodable on paper).
//
// The host keystore is loaded (by keystore_host.c) from
// keystore_entries.h, which is auto-generated from the firmware's REAL
// plaintext keystore asset,
// applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user
// (`Encryption: 0`, `KEYHEX:TYPE:NAME` per line, 216 entries) -- NOT the
// separate, actually-AES-encrypted `keeloq_mfcodes` asset. Real manufacturer
// names are loaded, so name-based classification (KIA/Beninca-by-name) and
// name-based decode identify both work against real data.
//
// Several manufacturer names in that real keystore are used by MULTIPLE
// entries of DIFFERENT `type` (e.g. "Beninca_ARC" appears at types 11, 2
// and 9; "BFT" at types 2 and 3). keeloq.c's encoder (gen_data) and decoder
// (check_remote_controller) both pick the encryption key AND the
// learning-type algorithm by scanning the keystore for the FIRST entry with
// an EXACT name match -- so testing a specific entry "by its real name"
// when that name is shared would silently exercise a DIFFERENT entry's
// key/type (or, when the first name-match is one of the dedicated-family
// types keeloq.c's own switch doesn't handle -- 5/9/10/11 -- silently
// encode garbage, since `create_data` still returns true even when no
// switch case matched and `hop` stayed 0). To test each entry independently
// and correctly, this tool renames every loaded entry, in place, to
// `<real name>_ix<index>` before testing -- unique per entry, and still
// classifiable (the KIA-prefix / "Beninca"-substring checks in the Task 1
// registry only look at the front/interior of the name, unaffected by the
// appended suffix). That rename is the only mutation this tool makes to the
// shared host keystore; it does not touch key or type. The matrix prints
// the real (pre-rename) name for readability.
//
// Per generic-encodable entry, the round trip is:
//   1. subghz_protocol_keeloq_create_data() builds a properly-encrypted
//      "capture" (Bit+Key fields) at a chosen Cnt=N using that entry's real
//      key/type -- the exact same primitive the real app's by-manufacturer
//      "Generate" flow uses (subghz_txrx_gen_keeloq_protocol() in
//      subghz_txrx_create_protocol_key.c).
//   2. That capture is replayed through the REAL encoder (deserialize +
//      yield), exactly like pressing "Send" on a loaded key file: keeloq.c
//      re-derives serial/btn/cnt from the Key (brute-force restricted to
//      our named entry via a written "Manufacture" field) and then
//      auto-advances the counter by the rolling-counter multiplier (1 on
//      host -- see host_shim.c) before generating the actual RF durations.
//   3. Those durations are fed into the REAL decoder. keystore->mfname is
//      reset to "" first so the decoder performs a genuine unrestricted
//      brute-force identify across every loaded entry (not just the one we
//      built) -- this is the actual coverage assertion, not a shortcut.
//   4. PASS requires: decoded MF == this entry's (renamed, unique) name AND
//      decoded Cnt == (N + 1) & 0xFFFF.
//
// Discriminator collisions: keeloq.c's check_decrypt validates only 4 (btn)
// + up to 8 (serial low byte) bits of a decrypted 32-bit hop -- a wrong key
// has a ~1/2048 chance of false-accepting per (wrong-key, algorithm) trial,
// and type-0 (UNKNOWN) keystore entries try 8 algorithm variants each during
// brute-force identify, so they dominate false-accepts. Each entry gets a
// PER-ENTRY-UNIQUE serial (0x100000 + index*prime) on the primary attempt,
// and up to 3 more attempts at different (serial, btn, cnt) if that reports
// a manufacture mismatch.
//
// That retry alone does not resolve every row: the real keystore contains
// several manufacturer names duplicated across types with the IDENTICAL raw
// key (e.g. "Leopard"/"Reff"/"Teco"/"ZX-730-750-1055"/"Faraon" each have a
// type-0 entry sharing one raw key with a later type-1/2 entry -- these
// look like deliberately-placed keystore test/placeholder rows, not
// independent real keys). For those, EVERY attempt "fails" the name
// comparison identically no matter the serial/btn/cnt, because the earlier
// (lower-index) same-key entry's broader UNKNOWN-case search (it tries 8
// algorithm variants per entry) is mathematically a 100%-correct decrypt
// too -- it's genuinely valid crypto, just attributed to a different
// catalogued name by array order. A few OTHER entries also lose to a
// DIFFERENT key's UNKNOWN-case search on every one of the 4 attempts --
// this is NOT a genuine key tie (the winner's raw key differs from the
// seeded entry's), and is treated as a real FAIL, not laundered (see below).
//
// After all retries exhaust with a "manufacture mismatch", this tool checks
// KEY EQUIVALENCE: does the winning (decoded) entry's raw key equal the
// seeded entry's own raw key? If yes, this is a genuine real-pipeline
// round trip through keeloq.c's actual encoder+decoder -- the decoder
// legitimately identified a different keystore ROW that happens to share
// an identical key, which is a property of the keystore data, not a bug --
// and, provided the counter also correctly advanced, this counts as PASS,
// clearly labelled `key-equivalent`. If the winning entry's key is
// DIFFERENT (a true cross-key discriminator false-accept, not a genuine
// tie) or the counter didn't advance correctly, that is a real FAIL: no
// bypass, no self-consistency shortcut. (An earlier version of this tool
// used a direct_crypto_verify() self-consistency check --
// encrypt(x)-then-decrypt(x) with the SAME key/algorithm -- as a
// tie-breaker; that was a tautology that could only fail if the base cipher
// primitive itself were broken, and it hardcoded the hop/plaintext layout
// by `type` alone, when the real keeloq.c selects that layout by
// MANUFACTURER NAME (a strcmp chain -- see keeloq.c's gen_data, ~lines
// 356-406 -- for `Mutanco_Mutancode`/`DTM_Neo`/`FAAC_RC,XT`/`Aprimatic`/etc,
// which get a 12-bit-serial or custom-parity layout the self-check never
// built). It did not go through the real encoder->decoder pipeline at all
// and has been removed.)
//
// Dedicated families (FaacSLH/KingGates/Jarolift/Beninca) print ROUTE-OK --
// their own encoder round-trip is out of this harness's v1 scope (they have
// their own protocol files with their own tests). decode_only and
// KlFamOrphan entries are reported as-is, no round trip attempted.
//
// Build: keeloq_coverage_build.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/protocols/keeloq.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/subghz_keystore_i.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>
#include <m-array.h>

#include "subghz_keeloq_registry.h" // Task 1

#define BASE_CNT 100u
#define ROLLING_MULT 1u // matches host_shim.c furi_hal_subghz_get_rolling_counter_mult()
#define DUR_CAP 8192
#define MAX_ATTEMPTS 4 // primary + up to 3 discriminator-collision retries
#define MAX_ENTRIES 512 // keystore has 216 entries; generous headroom

// Snapshot of every entry's raw key, captured before this tool renames
// entries to their unique `_ix<N>` form (see file header). Indexed by
// array position, so a decoded winner's `_ix<N>` suffix maps straight back
// to its real key for the key-equivalence check.
static uint64_t g_snap_key[MAX_ENTRIES];
static int g_snap_n = 0;

static void hit_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    (*(int*)ctx)++;
}

// Run the REAL encoder's deserialize->yield pipeline on a seeded
// FlipperFormat (mirrors "press Send on a loaded key"). Returns edge count.
static size_t run_encoder(FlipperFormat* ff, int* durs, size_t cap, const char** reason) {
    static char env;
    *reason = "";
    void* enc = subghz_protocol_keeloq_encoder.alloc((SubGhzEnvironment*)&env);
    flipper_format_rewind(ff);
    SubGhzProtocolStatus st = subghz_protocol_keeloq_encoder.deserialize(enc, ff);
    if(st != SubGhzProtocolStatusOk) {
        subghz_protocol_keeloq_encoder.free(enc);
        *reason = "deser-fail";
        return 0;
    }
    size_t n = 0;
    for(int g = 0; g < (int)cap; g++) {
        LevelDuration ld = subghz_protocol_keeloq_encoder.yield(enc);
        if(level_duration_is_reset(ld)) break;
        int d = level_duration_get_level(ld) ? (int)level_duration_get_duration(ld) :
                                                -(int)level_duration_get_duration(ld);
        if(n < cap) durs[n++] = d;
    }
    subghz_protocol_keeloq_encoder.free(enc);
    if(n == 0) *reason = "no-upload";
    return n;
}

// Feed duration edges into the REAL decoder; return the flattened
// get_string() output (empty if no frame was recognized).
static bool run_decoder(const int* durs, size_t n, char* flat, size_t flatcap) {
    static char env;
    flat[0] = 0;
    void* dec = subghz_protocol_keeloq_decoder.alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    int hit = 0;
    subghz_protocol_decoder_base_set_decoder_callback(base, hit_cb, &hit);
    for(size_t i = 0; i < n; i++) {
        int v = durs[i];
        subghz_protocol_keeloq_decoder.feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    // Real captures end in silence; the encoder's yield does not. Flush the
    // decoder's last frame with trailing low-level gaps so end-of-frame commits.
    for(int f = 0; f < 4 && !hit; f++) subghz_protocol_keeloq_decoder.feed(dec, false, 200000u);
    if(hit) {
        FuriString* o = furi_string_alloc();
        subghz_protocol_keeloq_decoder.get_string(dec, o);
        size_t j = 0;
        const char* c = furi_string_get_cstr(o);
        for(; c && *c && j < flatcap - 1; c++) flat[j++] = (*c == '\n' || *c == '\r') ? '|' : *c;
        flat[j] = 0;
        furi_string_free(o);
    }
    subghz_protocol_keeloq_decoder.free(dec);
    return hit != 0;
}

// Extract the token following a label ("MF:", "Cnt:") into out (trimmed word).
static bool field_after(const char* flat, const char* label, char* out, size_t cap) {
    const char* p = strstr(flat, label);
    if(!p) return false;
    p += strlen(label);
    while(*p == ' ') p++;
    size_t j = 0;
    while(*p && *p != ' ' && *p != '|' && j < cap - 1) out[j++] = *p++;
    out[j] = 0;
    return j > 0;
}

// Parse the trailing "_ix<N>" this tool's own renaming scheme appends to
// every entry's name, returning N. Used to map a decoded winner's name
// back to its keystore array index for the key-equivalence check.
static bool parse_ix_suffix(const char* name, int* out_idx) {
    const char* p = strstr(name, "_ix");
    if(!p) return false;
    p += 3;
    if(!isdigit((unsigned char)*p)) return false;
    *out_idx = (int)strtol(p, NULL, 10);
    return true;
}

// One full create_data -> encoder(deserialize+yield) -> decoder(feed+get_string)
// round trip for `name` at the given (serial, btn, cnt), through the REAL
// keeloq.c pipeline end to end. On success, *mf_tok / *decoded_cnt hold the
// decoded fields. Returns true iff the decoded manufacture name equals
// `name` and the decoded counter equals (cnt + ROLLING_MULT). On a
// "manufacture mismatch" (a different keystore entry won the brute-force
// identify), *mf_tok / *decoded_cnt still hold whatever the decoder
// actually reported, so the caller can apply the key-equivalence check.
static bool try_roundtrip(
    SubGhzKeystore* keystore,
    const char* name,
    uint32_t serial,
    uint8_t btn,
    uint16_t cnt,
    char* mf_tok,
    size_t mf_cap,
    uint32_t* decoded_cnt,
    const char** fail_reason) {
    static char env;
    *fail_reason = "";
    mf_tok[0] = 0;
    *decoded_cnt = 0;

    void* enc0 = subghz_protocol_keeloq_encoder.alloc((SubGhzEnvironment*)&env);
    FuriString* preset_name = furi_string_alloc();
    furi_string_cat_str(preset_name, "AM650");
    SubGhzRadioPreset preset = {
        .name = preset_name, .frequency = 433920000, .data = NULL, .data_size = 0};
    FlipperFormat* ff = flipper_format_string_alloc();
    bool built = subghz_protocol_keeloq_create_data(enc0, ff, serial, btn, cnt, name, &preset);
    subghz_protocol_keeloq_encoder.free(enc0);
    furi_string_free(preset_name);
    if(!built) {
        flipper_format_free(ff);
        *fail_reason = "create_data failed";
        return false;
    }

    // Mirrors subghz_txrx_gen_keeloq_protocol(): create_data does not itself
    // write "Manufacture"; the caller does.
    flipper_format_write_string_cstr(ff, "Manufacture", name);

    int durs[DUR_CAP];
    const char* reason;
    size_t nd = run_encoder(ff, durs, DUR_CAP, &reason);
    flipper_format_free(ff);
    if(nd == 0) {
        *fail_reason = reason;
        return false;
    }

    // Force a genuine full brute-force identify on decode -- do not let the
    // encoder-side match leak the answer to the decoder.
    keystore->mfname = "";
    keystore->kl_type = 0;

    char flat[1024];
    if(!run_decoder(durs, nd, flat, sizeof(flat))) {
        *fail_reason = "decoder silent";
        return false;
    }

    char cnt_tok[16] = "";
    field_after(flat, "MF:", mf_tok, mf_cap);
    field_after(flat, "Cnt:", cnt_tok, sizeof(cnt_tok));
    *decoded_cnt = (uint32_t)strtoul(cnt_tok, NULL, 16);

    uint32_t expected_cnt = ((uint32_t)cnt + ROLLING_MULT) & 0xFFFF;
    if(strcmp(mf_tok, name) != 0) {
        *fail_reason = "manufacture mismatch";
        return false;
    }
    if(*decoded_cnt != expected_cnt) {
        *fail_reason = "counter mismatch";
        return false;
    }
    return true;
}

// Deterministic, per-(attempt,idx)-varying (serial, btn, cnt) inputs. attempt
// 0 is the primary try (serial is per-entry-unique so distinct entries never
// share a discriminator by construction); attempts 1-3 are collision-retry
// fallbacks, each at a materially different serial/btn/cnt so a genuine
// crypto/registry bug (which would fail identically every time) is not
// confused with a rare discriminator collision (which will not reproduce at
// different inputs).
static void attempt_params(int attempt, int idx, uint32_t* serial, uint8_t* btn, uint16_t* cnt) {
    static const uint32_t base[MAX_ATTEMPTS] = {0x100000u, 0x200000u, 0x300000u, 0x400000u};
    // All non-zero so `idx` always moves `serial` on every attempt, including
    // the primary one.
    static const uint32_t mult[MAX_ATTEMPTS] = {1u, 7919u, 104729u, 15485867u};
    static const uint8_t btns[MAX_ATTEMPTS] = {1, 2, 4, 8};
    static const uint32_t cnt_off[MAX_ATTEMPTS] = {0u, 500u, 900u, 1300u};
    *serial = (base[attempt] + (uint32_t)idx * mult[attempt]) & 0x0FFFFFFFu;
    *btn = btns[attempt];
    *cnt = (uint16_t)(BASE_CNT + cnt_off[attempt] + (uint32_t)idx);
}

static const char* family_name(KeeloqFamily f) {
    switch(f) {
    case KlFamGeneric: return "Generic";
    case KlFamFaacSLH: return "FaacSLH";
    case KlFamKingGates: return "KingGates";
    case KlFamJarolift: return "Jarolift";
    case KlFamBeninca: return "Beninca";
    case KlFamKiaCar: return "KiaCar";
    case KlFamOrphan: return "Orphan";
    default: return "?";
    }
}

int main(void) {
    SubGhzKeystore* keystore = subghz_environment_get_keystore(NULL);
    SubGhzKeyArray_t* arr = subghz_keystore_get_data(keystore);
    size_t n_entries = SubGhzKeyArray_size(*arr);

    printf("KeeLoq host coverage matrix -- %zu keystore entries loaded\n", n_entries);
    printf("IDX\tTYPE\tFAMILY\tNAME\tCLASS\tRESULT\tDETAIL\n");
    printf("---\t----\t------\t----\t-----\t------\t------\n");

    // Pass 1: snapshot every entry's real key (for the key-equivalence check
    // below) and rename it to a globally-unique string derived from its real
    // name -- required for keeloq.c's exact-name keystore match to target
    // THIS entry, not an earlier identically-named one. Classification (pass
    // 2) uses this same, still KIA-prefix/Beninca-substring-detectable name,
    // since it's what the round trip actually exercises.
    {
        int i = 0;
        for
            M_EACH(e, *arr, SubGhzKeyArray_t) {
                if(i < MAX_ENTRIES) g_snap_key[i] = e->key;
                char orig_name[40];
                strncpy(orig_name, furi_string_get_cstr(e->name), sizeof(orig_name) - 1);
                orig_name[sizeof(orig_name) - 1] = 0;
                char uniq_name[48];
                snprintf(uniq_name, sizeof(uniq_name), "%s_ix%d", orig_name, i);
                furi_string_set_str(e->name, uniq_name);
                i++;
            }
        g_snap_n = i;
    }

    int total = 0, generic_pass = 0, generic_fail = 0, route_ok = 0, decode_only = 0,
        orphaned = 0, key_equiv_pass = 0;
    int types_seen[32] = {0};

    int idx = 0;
    for
        M_EACH(e, *arr, SubGhzKeyArray_t) {
            total++;

            const char* uniq_name = furi_string_get_cstr(e->name);
            // Strip the "_ixN" suffix back off for the printed NAME column.
            char orig_name[48];
            strncpy(orig_name, uniq_name, sizeof(orig_name) - 1);
            orig_name[sizeof(orig_name) - 1] = 0;
            char* suf = strstr(orig_name, "_ix");
            if(suf) *suf = 0;

            KeeloqTypeInfo info = subghz_keeloq_registry_type_info(e->type, uniq_name);
            const char* fam_s = family_name(info.family);
            if(e->type < 32) types_seen[e->type]++;

            if(info.encodable && info.family == KlFamGeneric) {
                char mf_tok[48];
                uint32_t decoded_cnt = 0;
                const char* fail_reason = "";
                bool ok = false;
                uint16_t last_cnt = 0;
                int attempts_made = 0;
                for(int a = 0; a < MAX_ATTEMPTS; a++) {
                    uint32_t serial;
                    uint8_t btn;
                    uint16_t cnt;
                    attempt_params(a, idx, &serial, &btn, &cnt);
                    last_cnt = cnt;
                    attempts_made++;
                    ok = try_roundtrip(
                        keystore,
                        uniq_name,
                        serial,
                        btn,
                        cnt,
                        mf_tok,
                        sizeof(mf_tok),
                        &decoded_cnt,
                        &fail_reason);
                    if(ok) break;
                    // Only a manufacture mismatch (discriminator collision or
                    // genuine key tie) is worth retrying; encode/decode
                    // failures are deterministic and retrying won't change
                    // them.
                    if(strcmp(fail_reason, "manufacture mismatch") != 0) break;
                }

                bool key_equiv = false;
                if(!ok && strcmp(fail_reason, "manufacture mismatch") == 0) {
                    // The decoder's full brute-force identify legitimately
                    // won on a DIFFERENT keystore row. Accept this as a real
                    // (not bypassed) pass only if that row shares this
                    // entry's own raw key -- i.e. it is a genuine keystore
                    // duplicate, and the SAME real decoder round trip this
                    // tool runs for every other entry still applies; only
                    // the winning NAME differs. A different key is a true
                    // false-accept, not a tie, and stays a FAIL.
                    int winner_idx;
                    if(parse_ix_suffix(mf_tok, &winner_idx) && winner_idx >= 0 &&
                       winner_idx < g_snap_n) {
                        uint64_t expected_cnt = ((uint32_t)last_cnt + ROLLING_MULT) & 0xFFFFu;
                        if(g_snap_key[winner_idx] == e->key && decoded_cnt == expected_cnt) {
                            key_equiv = true;
                            ok = true;
                        }
                    }
                }

                if(ok) {
                    generic_pass++;
                    if(key_equiv) {
                        key_equiv_pass++;
                        printf(
                            "%d\t%u\t%s\t%s\tgeneric-encodable\tPASS\tCnt:%04X (key-equivalent: decoded as %s, %d attempt(s))\n",
                            idx,
                            e->type,
                            fam_s,
                            orig_name,
                            decoded_cnt,
                            mf_tok,
                            attempts_made);
                    } else {
                        printf(
                            "%d\t%u\t%s\t%s\tgeneric-encodable\tPASS\tCnt:%04X%s\n",
                            idx,
                            e->type,
                            fam_s,
                            orig_name,
                            decoded_cnt,
                            attempts_made > 1 ? " (after discriminator-collision retry)" : "");
                    }
                } else {
                    generic_fail++;
                    printf(
                        "%d\t%u\t%s\t%s\tgeneric-encodable\tFAIL\t%s (MF:%s Cnt:%04X, %d attempt(s), key-equivalence check also failed)\n",
                        idx,
                        e->type,
                        fam_s,
                        orig_name,
                        fail_reason,
                        mf_tok,
                        decoded_cnt,
                        attempts_made);
                }
            } else if(info.encodable) {
                // Dedicated family with its own encoder (FAAC/KingGates/Jarolift/
                // Beninca) -- round trip covered by that protocol's own tests.
                route_ok++;
                printf(
                    "%d\t%u\t%s\t%s\tROUTE:%s\tROUTE-OK\tdedicated encoder round-trip out of v1 scope\n",
                    idx,
                    e->type,
                    fam_s,
                    orig_name,
                    fam_s);
            } else if(info.family == KlFamOrphan) {
                orphaned++;
                printf(
                    "%d\t%u\t%s\t%s\torphan\tORPHANED\tno known generator route\n",
                    idx,
                    e->type,
                    fam_s,
                    orig_name);
            } else {
                decode_only++;
                printf(
                    "%d\t%u\t%s\t%s\tdecode-only\tDECODE-ONLY\tcatalog-generate not v1-supported\n",
                    idx,
                    e->type,
                    fam_s,
                    orig_name);
            }
            idx++;
        }

    printf("\n-- learning types present in this keystore snapshot --\n");
    for(int t = 0; t < 32; t++) {
        if(types_seen[t])
            printf("type %2d: %d entr%s\n", t, types_seen[t], types_seen[t] == 1 ? "y" : "ies");
    }

    printf(
        "\nSUMMARY: total=%d generic_PASS=%d (of which key-equivalent=%d) generic_FAIL=%d ROUTE-OK=%d DECODE-ONLY=%d ORPHANED=%d\n",
        total,
        generic_pass,
        key_equiv_pass,
        generic_fail,
        route_ok,
        decode_only,
        orphaned);

    return generic_fail > 0 ? 1 : 0;
}
