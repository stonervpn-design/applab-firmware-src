# KeeLoq Master-Key Console — Final Report (Task 6)

**Feature branch:** `feat/keeloq-no-desync-clone`
**Report HEAD:** `64886d6` (Task 5 commit; Task 6 adds only this report)
**Status:** DONE — all 6 tasks complete, all verification green, no regressions.

## What shipped (Tasks 1–5)

| Task | Commit(s) | Summary |
|---|---|---|
| 1 | `36fc420`, `adeff61` | New `subghz_keeloq_registry.{h,c}` — a single table-driven classifier (`subghz_keeloq_registry_type_info`) mapping every KeeLoq learning-type (0–15) + manufacturer name to a `KeeloqFamily` (Generic/FaacSLH/KingGates/Jarolift/Beninca/KiaCar/Orphan), an `encodable` flag, and a `GenType` route. `GenType` enum extracted to its own leaf header (`subghz_gen_type.h`) so the registry stays furi-free and host-testable. Host unit test covers every routing branch including the KIA/Beninca name-disambiguation matrix and the orphan default case. |
| 2 | `9014082`, `7d95f27`, `a48b948`, `5ec2988` | Host coverage harness (`tools/decoder_host_test/keeloq_coverage.c`) that runs the **real** `keeloq.c` encoder+decoder (no stubs) against all 216 entries of the real plaintext keystore asset (`keeloq_mfcodes_user`). Went through 2 fix rounds after review: removed a tautological "direct crypto verify" shortcut, then split the metric in two — **Metric A (EMULATE-CRYPTO)**: restricted decode with the entry's own key, this is the exit-gating pass/fail check; **Metric B (DICTIONARY-ID)**: unrestricted keystore-wide brute-force identify, informational, feeds Task 5's honest false-accept caveat. |
| 3 | `f06392b` | Un-hid 36 previously-allow-listed-out encoder-ready keys (ERREKA/PUJOL/AERF/SIMPLE_JCM, types 12–15) in the "KL: By Manufacturer" picker by deleting its local stale hardcoded allow-list and routing through the Task 1 registry instead (`subghz_keeloq_registry_type_encodable`). |
| 4 | `61acddb`, `8007889` | New "KeeLoq Console (all keys)" scene reachable from Car Keys — browses **every** keystore entry (all families) grouped by section, routes encodable entries into the existing SetSerial→...→generate chain, shows decode-only entries as an info popup. Fix round addressed 1 Critical (missing `#ifndef PUBLIC_BUILD` wrap — would have broken public compile) + 2 Important (BenincaVA type-11 registry mis-classification fixed to decode-only since the protocol only reads type-9 keys; "Clone (no desync)" menu option now gated to `gen_info->type == GenKeeloq` so it can't reinterpret a non-KeeLoq union as bogus KeeLoq fields). |
| — | `7eb7a0a`, `5850896` | Adjacent car-fob branch work folded in: 7 whole-branch review findings fixed, and a genuine pre-existing public-build car-material leak (`subghz_fob_buttons.c`'s `FOB_ENTRY` table had zero `PUBLIC_BUILD` guards) found and fixed — 18 car RKE protocol names were leaking into the public binary. Independently re-verified clean. |
| 5 | `64886d6` | Signal-ID screen: for a rolling KeeLoq capture, labels the candidate maker + family (via the Task 1 registry) instead of a generic "not automotive" fallback, and adds a "KeyAtk" spoke (Right button, confirmed free for this capture class) that surfaces the decoder's **already-computed** dictionary-attack result (no new brute-force engine written) with an explicit false-accept-rate caveat on match and the honest 2^64-infeasible message on no-match. |

All of Tasks 1–5's new/changed code is inside `#ifndef PUBLIC_BUILD` guards (verified by symbol-absence in the public `.map` file, not just a `grep` of the source).

## Coverage numbers (of 216 real keystore entries, `keystore_entries.h` regenerated from `keeloq_mfcodes_user`)

Fresh run this task, `tools/decoder_host_test/keeloq_coverage`:

```
SUMMARY: total=216 | EMULATE-GATE generic_PASS=179 generic_FAIL=0 (exit gate)
       | DICTIONARY-ID unique=143 key-equiv=13 false-accept=23 (informational)
       | ROUTE-OK=5 DECODE-ONLY=32 ORPHANED=0
```

- **Metric A — EMULATE-CRYPTO (exit-gating):** 179 generic-encodable entries, all round-trip
  correctly through the real encoder → RF durations → real decoder using **that entry's own
  key** (restricted decode). `generic_FAIL=0`. This is the number that matters for "does the
  console's Generate flow actually work" — un-hidden by Task 3, exercised end-to-end by Task 2.
- **Metric B — DICTIONARY-ID (informational, unrestricted keystore-wide brute-force identify):**
  of those same 179 generated frames, 143 identify back to a **unique** correct manufacturer,
  13 identify to a **key-equivalent** entry (a different keystore row that happens to share the
  identical raw key — placeholder/test-vector duplication in the source asset, not a crypto
  bug), and **23 are genuine false-accepts** — KeeLoq's decode-side discriminator is only
  ~12 bits (4-bit button + up to 8-bit serial-low-byte match on a decrypted 32-bit hop), so a
  216-entry keystore has real collision pressure. This 23/179 figure is exactly what Task 5's
  dictionary-attack UI caveats on every match ("216-key match has a real false-accept rate —
  confirm before relying on it").
- **ROUTE-OK=5**: the dedicated non-generic families (FaacSLH, KingGates, Jarolift, Beninca
  type-9) — their own encoder round-trip is out of this harness's v1 scope; they're routed and
  reachable (verified by Task 4) but not host-round-tripped by this tool.
- **DECODE-ONLY=32**: type-0 (dictionary/legacy, no independent encoder path) entries plus the
  KIA/Hyundai type-9/10/11 name-matched entries (real cars — decode/identify only, no
  synthesis, by design).
- **ORPHANED=0**: every one of the 216 real entries now classifies into a known family (the
  old 83-entry nameless snapshot had 2 orphans that were an artifact of missing names, not a
  registry gap — resolved when Task 2's fix round regenerated the keystore with real names).

## Honest limits (not fixed by this feature, by design)

1. **2^64 unknown-key space is out of scope.** The dictionary attack (Task 5) can only ever
   identify a capture against the ~216 keys physically present in the keystore asset. It is not,
   and does not claim to be, a general KeeLoq key-recovery attack — the no-match UI message says
   this explicitly ("Exhaustive key recovery is 2^64 — not feasible on-device.").
2. **Type-0 (legacy/dictionary) entries are decode-only in v1.** These 32 entries (of 216) have
   no independent encoder path in `keeloq.c` — they can be identified/decoded but not
   synthesized/generated from the console. This was a pre-existing protocol-level limitation,
   not something Task 3/4 introduced.
3. **~23/179 (13%) of the generic-encodable keystore keys have a discriminator false-accept
   partner.** This is inherent to KeeLoq's short match window at 216-key keystore density, not a
   bug in the registry, the coverage harness, or the console. It is surfaced honestly rather than
   hidden: Task 5's Signal-ID dictionary-attack result always shows the false-accept caveat, and
   the coverage numbers above report it as its own informational metric instead of folding it
   into the pass/fail gate.
4. **The dedicated families' (FaacSLH/KingGates/Jarolift/Beninca) own encoder round-trip is not
   host-verified** — Task 2's harness confirms they *route* correctly (ROUTE-OK) via Task 4's
   wiring, but does not run their protocol-specific encoder through a full generate→decode
   cycle the way it does for the 179 generic entries. Out of this feature's v1 scope per the
   original task briefs.
5. **BenincaVA (type 11) is decode-only, not a registry oversight.** `beninca_arc.c`'s AES-key
   lookup only ever reads the keystore's type-9 entry; a type-11-named entry routed through the
   console would silently reuse the type-9 key. The registry was fixed (Task 4 fix round) to
   mark type-10/11 Beninca-by-name as `decode_only`, not `encodable` — this is the *correct*
   fix, and the coverage tool's Metric-run and the report above already reflect it (Beninca
   ROUTE-OK count is for the genuine type-9 route only).

## Deferred Minors (flagged in review, intentionally not blocking Task 6)

From Task 5's review (3 Minors, deferred to this final report rather than a fix round):

- **(a) DictAttack result screen has no re-added navigation buttons** — after pressing "KeyAtk"
  and viewing the result, only the hardware Back button returns to the Signal-ID view (no
  on-screen "Try again"/spoke re-added). UX polish, not a correctness issue.
- **(b) Keystore linear-scan per press** — `subghz_scene_analyze_keeloq_family()` does a linear
  scan of the in-memory keystore (up to ~224 entries) on every Signal-ID display and again on
  every KeyAtk press. Cheap in absolute terms (same keystore already resident, same scan style
  `subghz_scene_keeloq_console.c` already uses) but not cached.
- **(c) BFT/Erreka `" Sd:..."` suffix-stripping logic is duplicated** between
  `subghz_scene_rolling_analyzer.c` (existing) and Task 5's new `subghz_scene_analyze.c` helper,
  rather than factored into a shared header. Premature to hoist for a two-caller, five-line
  helper; flagged for a future cleanup if a third caller appears.

None of these three affect correctness, security-gating (public/private), or the coverage
numbers above.

## Build results — all 3 targets, all fresh-verified (no stale logs)

Per the mandatory anti-stale-log method (delete the build dir, invoke the `.bat` by absolute
path, confirm the log's mtime is current before trusting its content — this discipline exists
because an earlier session in this feature's history had a `cd`-chained background invocation
silently fail while leaving a stale prior-run `SENTINEL_OK` in the log):

| Target | Log | mtime | Result |
|---|---|---|---|
| Private m5stick_s3 (`build_m5stick_s3.bat`) | `build_m5stick_s3.log` | 2026-08-12 19:12:04 | `SENTINEL_OK` (cited from Task 5's fresh-verified run at HEAD `64886d6`, not re-run in Task 6 per caller instruction) |
| Public m5stick_s3 (`build_m5stick_s3_public.bat`) | `build_m5stick_s3_public.log` | 2026-08-12 19:14:11 | `SENTINEL_VERIFY_NOCAR → PASS: no car material in ...public-merged.bin → SENTINEL_OK` (cited, same run) |
| Private Cardputer-ADV (`build_adv_only_private.bat`) | `build_adv_only_private.log` | 2026-08-12 19:26 | `SENTINEL_OK` — **built fresh in this task** (`build_cardputer_adv/` deleted first, invoked by absolute path, log tail confirms `SENTINEL_MERGE` → `Wrote 0x2f4970 bytes to file Flipper-cardputer_adv-merged.bin` → `SENTINEL_OK`) |

The public exclusion check (no KeeLoq console / dictionary-attack / car-fob material in the
public binary) is the load-bearing security gate for this feature — it PASSED on the
genuinely-fresh build cited above, after the leak-fix commit (`5850896`) that guarded
`subghz_fob_buttons.c`'s previously-unguarded car table.

## Host test results (this task, fresh WSL runs)

1. **Registry unit test** (`tests/test_keeloq_registry.c`):
   ```
   test_keeloq_registry: ALL PASS
   ```
2. **Coverage matrix** (`tools/decoder_host_test/keeloq_coverage`):
   ```
   SUMMARY: total=216 | EMULATE-GATE generic_PASS=179 generic_FAIL=0 (exit gate)
          | DICTIONARY-ID unique=143 key-equiv=13 false-accept=23 (informational)
          | ROUTE-OK=5 DECODE-ONLY=32 ORPHANED=0
   ```
3. **Car-fob decoder regression** (`tools/decoder_host_test/fob_sweep`) — proves the shared
   keystore/registry changes in this feature did **not** regress the pre-existing car-fob
   decoders:
   ```
   SUMMARY(synthetic): total=80 PASS=28 FAIL=43 SEED-FAIL=9
   SUMMARY(capture-seeded): PASS=16 NON-PASS=3
   ```
   Matches the expected/unchanged baseline (28/80 synthetic, 16/3 capture-seeded) exactly.

Note: the Beninca registry fix (Task 4) means the coverage tool's classification of type-10/11
Beninca-by-name entries changed from `ROUTE-OK` to `DECODE-ONLY` relative to Task 2's original
run — this is the **expected, correct** consequence of that fix (see "Honest limits" #5 above),
not a regression.

## Hardware testing — deferred

This entire feature was verified via **host-only crypto round-trips (WSL/gcc against the real
`lib/subghz/protocols` source) and firmware build verification (ESP-IDF, both private targets +
the public exclusion gate)**. No on-device flash/UI walkthrough was performed as part of Tasks
1–6. The logical reachability of every new scene/route was traced against source (documented in
each task's report — e.g. Task 4's "Manual reachability check" section), but pressing real
buttons on real M5Stick S3 / Cardputer-ADV hardware to confirm the KeeLoq Console browse→generate
flow, the Signal-ID KeyAtk spoke, and the un-hidden 36-key manufacturer picker remains
**outstanding** and should be the next step before considering this feature fully shipped.

## Next step (per the SDD ledger)

A final whole-branch review (most-capable model, full branch diff including the adjacent
car-fob fixes and the leak fix) is planned after this report, pointed specifically at the 3
deferred Minors above.
