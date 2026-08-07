# P5 — Multi-frame Signal-ID Hardening (progress ledger)

Plan: harden the RF signal-identify arc with a K-of-N multi-frame vote so one
noisy frame can't mislabel. Private-only (#ifndef PUBLIC_BUILD via the FA worker
call-site). Canonical tree: src_m5stick_s3.

## Task 1 — pure K-of-N aggregator + host test — COMPLETE
- NEW applications/main/subghz/helpers/subghz_sig_aggregate.c (pure: signal_fingerprint.h + string.h)
- decl in subghz_sig_identify.h
- tests/test_subghz_sig_aggregate.c (7 cases: 3-agree, 2-of-3, all-distinct, below-floor, valid+UNKNOWN, empty)
- tools/sig_aggregate/aggregate_replica.py (Python mirror) — ALL PASS
- xtensa -fsyntax-only on aggregate.c — SYNTAX_OK

## Task 2 — capture N=3 frames + vote — COMPLETE
- subghz_sig_identify.c: SIG_ID_FRAMES=3, SIG_ID_MIN_VOTES=2; per-frame idle/preset/setfreq/flush/rx/delay/stop loop; aggregate() call

## Task 3 — build private + public gate
- private build (build_m5stick_s3.bat): exit 0; nm shows subghz_sig_aggregate + _identify_run + _from_samples PRESENT — DONE
- public gate (build_m5stick_s3_public.bat): IN PROGRESS — expect 0 sig symbols (arc gc'd) + car-free

## Task 3 — RESULT (COMPLETE, gate GREEN)
- private ELF: subghz_sig_aggregate + subghz_sig_identify_run + _from_samples PRESENT
- public gate:
  - private-arc symbols (aggregate/identify/mod_detect/chip_identify) in public = 0
  - signal_fingerprint_classify present in public = SANCTIONED inert stub
    (signal_fingerprint.c #ifdef PUBLIC_BUILD -> {"",NULL} table -> classify()==UNKNOWN)
  - objdump rodata proof: private obj carries Audi/BMW/Chevrolet/StarLine; public obj carries only "UNKNOWN"
  - car-free: verify_public_no_car.py in-build -> "PASS: no car material"; merged bin retained (no SENTINEL_CARKEY_LEAK)
- NOTE: grep -a is unreliable on binaries in this shell (even known strings miss);
  use nm / objdump -s for symbol+rodata checks, and verify_public_no_car.py for the car gate.

P5 v1 (multi-frame K-of-N signal-ID hardening) DONE.
Deferred: P5b extend fingerprint DB beyond automotive to general ISM (needs redo of failed research).
