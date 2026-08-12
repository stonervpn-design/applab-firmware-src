// KeeLoq master-key console -- Task 2: host coverage matrix.
//
// Proves, for every entry in the host keystore (keystore_host.c), that Task
// 1's registry classification (subghz_keeloq_registry_type_info) matches
// what the REAL lib/subghz/protocols/keeloq.c encoder+decoder actually do.
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
// and 9; "BFT" at types 2 and 3), and a few entries share an IDENTICAL raw
// key across different catalogued names/types (e.g. "Leopard" has a type-0
// AND a type-1 row with the same key) -- these look like deliberately
// placed keystore test/placeholder rows. keeloq.c's encoder (gen_data) and
// decoder (check_remote_controller) both pick the encryption key AND the
// learning-type algorithm by scanning the keystore for the FIRST entry with
// an EXACT name match -- so testing a specific entry "by its real name"
// when that name is shared would silently exercise a DIFFERENT entry's
// key/type. To test each entry independently and correctly, this tool
// renames every loaded entry, in place, to `<real name>_ix<index>` before
// testing -- unique per entry, and still classifiable (the KIA-prefix /
// "Beninca"-substring checks in the Task 1 registry only look at the
// front/interior of the name, unaffected by the appended suffix). That
// rename is the only mutation this tool makes to the shared host keystore;
// it does not touch key or type. The matrix prints the real (pre-rename)
// name for readability.
//
// TWO SEPARATE METRICS are reported per generic-encodable entry, from a
// SINGLE real encode (one subghz_protocol_keeloq_create_data() capture at
// Cnt=N, replayed through the REAL encoder deserialize+yield -- exactly
// like pressing "Send" on a loaded key file, which auto-advances the
// counter by the rolling-counter multiplier before generating the RF
// durations -- see host_shim.c for the multiplier value):
//
//   METRIC A -- EMULATE-CRYPTO (this is what gates the exit code).
//   Decode the SAME durations with keystore->mfname RESTRICTED to this
//   entry's own (unique, renamed) name -- i.e. the brute-force identify can
//   only ever try this ONE entry's key/type, exactly like a real car only
//   ever tries its own paired fob's key. This is NOT a tautology: it still
//   goes through keeloq.c's real per-type encode dispatch (gen_data's
//   switch) and real per-type decode dispatch
//   (check_remote_controller_selector's switch) -- if the plaintext layout,
//   the learning-type derivation, or the counter-advance logic disagreed
//   between encode and decode for this entry's type, restricting the scan
//   to just this one key would NOT save it; check_decrypt would fail and
//   this row would FAIL. PASS requires decoded MF == this entry's name AND
//   decoded Cnt == (N + rolling-counter-mult) & 0xFFFF. A Metric-A failure
//   is a genuine crypto/registry bug -- no bypass. Expected: every
//   generic-encodable row passes (each key/type pair only ever competes
//   against itself).
//
//   METRIC B -- DICTIONARY-ID (informational; does NOT gate the exit code).
//   Decode the SAME durations again, fresh, with keystore->mfname reset to
//   "" -- a genuine UNRESTRICTED brute-force identify across all 216
//   entries, exactly what an attacker holding only the RF capture (not the
//   fob) would have to do. keeloq.c's check_decrypt validates only 4 (btn)
//   + up to 8 (serial low byte) bits of a decrypted 32-bit hop -- a wrong
//   key has a ~1/2048 chance of false-accepting per (wrong-key, algorithm)
//   trial, and type-0 (UNKNOWN) keystore entries try 8 algorithm variants
//   each, so they dominate false-accepts; this real keystore's placeholder/
//   test keys (several near-identical or literally duplicated -- see
//   keystore_entries.h) make this worse than a uniformly-random keystore
//   would. Three outcomes, all informational:
//     - unique:         decoded MF == this entry's own name.
//     - key-equivalent:  decoded MF is a DIFFERENT entry, but that entry's
//                        raw key is IDENTICAL to this one's -- a genuine
//                        keystore duplicate, correctly identified to a
//                        same-key maker (not a bug, not this tool's doing).
//     - false-accept:    decoded MF is a different entry with a DIFFERENT
//                        raw key -- KeeLoq's short discriminator produced a
//                        wrong answer. This is a real, inherent limitation
//                        of brute-force dictionary identify at 216 keys
//                        (worsened by this keystore's patterned placeholder
//                        keys), NOT a defect in encode/emulate (Metric A),
//                        and it directly informs Task 5's dictionary
//                        attack, which will need its own confidence/
//                        disambiguation strategy on top of a raw brute
//                        force.
//   Every false-accept is listed by name+key in a dedicated section of the
//   output so Task 5 has concrete evidence, not just a count.
//
// (An earlier version of this tool used a direct_crypto_verify()
// self-consistency check -- encrypt(x)-then-decrypt(x) with the SAME
// key/algorithm -- as a Metric-B tie-breaker folded into the pass count;
// that was a tautology that could only fail if the base cipher primitive
// itself were broken, it never touched keeloq.c's real encoder/decoder at
// all, and it hardcoded the hop/plaintext layout by `type` alone when real
// keeloq.c actually selects that layout by MANUFACTURER NAME -- see
// gen_data, ~lines 356-406. It has been removed. Metric A now supplies the
// "does this entry's own crypto really work" proof, honestly, through the
// real pipeline; Metric B is reported as what it is -- a dictionary-ID
// limitation, not folded into the pass/fail gate.)
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
#define MAX_ENTRIES 512 // keystore has 216 entries; generous headroom
#define MAX_FALSE_ACCEPTS 64

// Snapshot of every entry's raw key, captured before this tool renames
// entries to their unique `_ix<N>` form (see file header). Indexed by
// array position, so a Metric-B decoded winner's `_ix<N>` suffix maps
// straight back to its real key for the key-equivalence check.
static uint64_t g_snap_key[MAX_ENTRIES];
static int g_snap_n = 0;

// Metric-B false-accept evidence, collected for the dedicated report
// section (Task 5 needs concrete name+key pairs, not just a count).
typedef struct {
    char seeded_name[48];
    uint64_t seeded_key;
    char winner_name[48];
    uint64_t winner_key;
} FalseAccept;
static FalseAccept g_false_accepts[MAX_FALSE_ACCEPTS];
static int g_false_accepts_n = 0;

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

// Feed duration edges into the REAL decoder (fresh instance every call, so
// Metric A and Metric B never share decoder state); return the flattened
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
// every entry's name, returning N. Used to map a Metric-B decoded winner's
// name back to its keystore array index for the key-equivalence check.
static bool parse_ix_suffix(const char* name, int* out_idx) {
    const char* p = strstr(name, "_ix");
    if(!p) return false;
    p += 3;
    if(!isdigit((unsigned char)*p)) return false;
    *out_idx = (int)strtol(p, NULL, 10);
    return true;
}

// Decode `durs` once with keystore->mfname forced to `mfname_scope` (an
// empty string means an unrestricted full-keystore brute force; a specific
// name restricts the scan to that ONE entry). Fills *mf_tok / *decoded_cnt
// from whatever the decoder actually reports; returns false only if the
// decoder never recognized a frame at all.
static bool decode_scoped(
    SubGhzKeystore* keystore,
    const char* mfname_scope,
    const int* durs,
    size_t nd,
    char* mf_tok,
    size_t mf_cap,
    uint32_t* decoded_cnt) {
    keystore->mfname = mfname_scope;
    keystore->kl_type = 0;
    mf_tok[0] = 0;
    *decoded_cnt = 0;
    char flat[1024];
    if(!run_decoder(durs, nd, flat, sizeof(flat))) return false;
    char cnt_tok[16] = "";
    field_after(flat, "MF:", mf_tok, mf_cap);
    field_after(flat, "Cnt:", cnt_tok, sizeof(cnt_tok));
    *decoded_cnt = (uint32_t)strtoul(cnt_tok, NULL, 16);
    return true;
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
    printf("IDX\tTYPE\tFAMILY\tNAME\tCLASS\tRESULT(A)\tDETAIL\n");
    printf("---\t----\t------\t----\t-----\t---------\t------\n");

    // Pass 1: snapshot every entry's real key (for Metric B's key-equivalence
    // check) and rename it to a globally-unique string derived from its real
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

    int total = 0, route_ok = 0, decode_only = 0, orphaned = 0;
    // Metric A (gates exit):
    int a_pass = 0, a_fail = 0;
    // Metric B (informational only):
    int b_unique = 0, b_key_equiv = 0, b_false_accept = 0;
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
                uint32_t serial = (0x100000u + (uint32_t)idx) & 0x0FFFFFFFu;
                uint8_t btn = 1;
                uint16_t cnt = (uint16_t)BASE_CNT;
                uint32_t expected_cnt = ((uint32_t)cnt + ROLLING_MULT) & 0xFFFFu;

                // ---- single REAL encode: create_data -> encoder deserialize+yield ----
                static char env_a;
                void* enc0 = subghz_protocol_keeloq_encoder.alloc((SubGhzEnvironment*)&env_a);
                FuriString* preset_name = furi_string_alloc();
                furi_string_cat_str(preset_name, "AM650");
                SubGhzRadioPreset preset = {
                    .name = preset_name, .frequency = 433920000, .data = NULL, .data_size = 0};
                FlipperFormat* ff = flipper_format_string_alloc();
                bool built = subghz_protocol_keeloq_create_data(
                    enc0, ff, serial, btn, cnt, uniq_name, &preset);
                subghz_protocol_keeloq_encoder.free(enc0);
                furi_string_free(preset_name);

                bool a_ok = false, b_hit_unique = false, b_hit_keyequiv = false;
                char a_mf[48] = "", b_mf[48] = "";
                uint32_t a_cnt = 0, b_cnt = 0;
                const char* fail_reason = "";

                if(!built) {
                    fail_reason = "create_data failed";
                } else {
                    flipper_format_write_string_cstr(ff, "Manufacture", uniq_name);
                    int durs[DUR_CAP];
                    const char* reason;
                    size_t nd = run_encoder(ff, durs, DUR_CAP, &reason);
                    flipper_format_free(ff);
                    if(nd == 0) {
                        fail_reason = reason;
                    } else {
                        // ---- Metric A: decode RESTRICTED to this entry's OWN name/key ----
                        // (a real car only ever tries its own paired fob's key)
                        bool a_hit =
                            decode_scoped(keystore, uniq_name, durs, nd, a_mf, sizeof(a_mf), &a_cnt);
                        if(!a_hit) {
                            fail_reason = "decoder silent";
                        } else if(strcmp(a_mf, uniq_name) != 0) {
                            fail_reason = "manufacture mismatch"; // should not happen: scan is restricted
                        } else if(a_cnt != expected_cnt) {
                            fail_reason = "counter mismatch";
                        } else {
                            a_ok = true;
                        }

                        // ---- Metric B: decode UNRESTRICTED (genuine full brute force) ----
                        bool b_hit = decode_scoped(keystore, "", durs, nd, b_mf, sizeof(b_mf), &b_cnt);
                        if(b_hit) {
                            if(strcmp(b_mf, uniq_name) == 0 && b_cnt == expected_cnt) {
                                b_hit_unique = true;
                            } else {
                                int widx;
                                if(parse_ix_suffix(b_mf, &widx) && widx >= 0 && widx < g_snap_n &&
                                   g_snap_key[widx] == e->key && b_cnt == expected_cnt) {
                                    b_hit_keyequiv = true;
                                }
                            }
                        }
                    }
                }

                if(a_ok) {
                    a_pass++;
                } else {
                    a_fail++;
                }

                const char* b_label;
                if(!a_ok) {
                    b_label = "n/a (encode/decode failed)";
                } else if(b_hit_unique) {
                    b_unique++;
                    b_label = "unique";
                } else if(b_hit_keyequiv) {
                    b_key_equiv++;
                    b_label = "key-equivalent";
                } else {
                    b_false_accept++;
                    b_label = "FALSE-ACCEPT";
                    if(g_false_accepts_n < MAX_FALSE_ACCEPTS) {
                        FalseAccept* fa = &g_false_accepts[g_false_accepts_n++];
                        strncpy(fa->seeded_name, orig_name, sizeof(fa->seeded_name) - 1);
                        fa->seeded_name[sizeof(fa->seeded_name) - 1] = 0;
                        fa->seeded_key = e->key;
                        strncpy(fa->winner_name, b_mf, sizeof(fa->winner_name) - 1);
                        fa->winner_name[sizeof(fa->winner_name) - 1] = 0;
                        int widx;
                        fa->winner_key =
                            (parse_ix_suffix(b_mf, &widx) && widx >= 0 && widx < g_snap_n) ?
                                g_snap_key[widx] :
                                0;
                    }
                }

                if(a_ok) {
                    // "unique" needs no extra detail; key-equivalent/false-accept
                    // both name the row's actual decoded winner.
                    bool show_winner = !b_hit_unique;
                    printf(
                        "%d\t%u\t%s\t%s\tgeneric-encodable\tPASS\tCnt:%04X | DICTIONARY-ID: %s%s%s\n",
                        idx,
                        e->type,
                        fam_s,
                        orig_name,
                        a_cnt,
                        b_label,
                        show_winner ? " decoded as " : "",
                        show_winner ? b_mf : "");
                } else {
                    printf(
                        "%d\t%u\t%s\t%s\tgeneric-encodable\tFAIL\t%s (MF:%s Cnt:%04X) -- Metric A gate failure, genuine bug\n",
                        idx,
                        e->type,
                        fam_s,
                        orig_name,
                        fail_reason,
                        a_mf,
                        a_cnt);
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

    printf("\n-- DICTIONARY-ID false-accepts (Metric B, informational, feeds Task 5) --\n");
    if(g_false_accepts_n == 0) {
        printf("(none)\n");
    } else {
        printf(
            "KeeLoq's check_decrypt validates only ~12 bits (4 btn + up to 8 serial) of a\n"
            "decrypted 32-bit hop, so a wrong key has a ~1/2048 false-accept chance per\n"
            "trial; type-0 (UNKNOWN) entries try 8 algorithm variants each, so they\n"
            "dominate. At 216 keys -- several of them patterned/near-duplicate placeholder\n"
            "test keys in this specific keystore, not independently random -- a handful of\n"
            "false-accepts is expected. This is inherent to brute-force dictionary ID, NOT\n"
            "a defect in encode/emulate (Metric A passed for every one of these rows), and\n"
            "it is exactly the kind of ambiguity Task 5's dictionary attack will need its\n"
            "own confidence/disambiguation strategy for.\n");
        for(int i = 0; i < g_false_accepts_n; i++) {
            FalseAccept* fa = &g_false_accepts[i];
            printf(
                "  %s (key %016llX) -> decoded as %s (key %016llX)\n",
                fa->seeded_name,
                (unsigned long long)fa->seeded_key,
                fa->winner_name,
                (unsigned long long)fa->winner_key);
        }
    }

    printf(
        "\nSUMMARY: total=%d | EMULATE-GATE generic_PASS=%d generic_FAIL=%d (exit gate) | "
        "DICTIONARY-ID unique=%d key-equiv=%d false-accept=%d (informational) | "
        "ROUTE-OK=%d DECODE-ONLY=%d ORPHANED=%d\n",
        total,
        a_pass,
        a_fail,
        b_unique,
        b_key_equiv,
        b_false_accept,
        route_ok,
        decode_only,
        orphaned);

    return a_fail > 0 ? 1 : 0;
}
