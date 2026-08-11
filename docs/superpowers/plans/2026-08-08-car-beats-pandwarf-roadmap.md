# Car Suite vs PandwaRF Rogue Pro — Roadmap (2026-08-08)

Source: workflow wf_ebacbd8f (11 agents: PandwaRF car recon + fzcard car inventory + RKE SOTA → ideate → adversarial verify → synth). PRIVATE-only; authorized research.

## Headline (honest)
**fzcard has already decisively beaten PandwaRF Rogue Pro on cars.** The Rogue Pro is a sub-Ghz-only CC1111 dongle tethered to a phone whose entire rolling-code story is the paid, account-gated, internet-dependent **Kaiju cloud** (car keyfob support shipped "alpha", separate paid add-on). fzcard runs the full **capture → make/model-ID → attack** chain **standalone + offline** on ESP32-S3, breaks KeeLoq on-radio vs a 224-key private keystore, and clones a KeeLoq fob without desyncing it — no cloud. The honest next move is to **BANK the lead (validation + evidence)**, not chase features.

## Already ahead of PandwaRF
1. **Decoder breadth + make/model classification** — ~27 OEM RKE families (ProtoPirate) + KeeLoq/VAG/Hitag-2/PSA/Hyundai-TL stacks that decode AND classify. PandwaRF only IDs fixed-code encoder chips (EV1527/HT12E/PT226x), no OEM fob families.
2. **On-device OFFLINE crypto** — byte-exact KeeLoq cipher, 224-key keystore + emulate, magic-XOR/secure-learning variants, verified Hitag-2, VAG+PSA encoders, on-device PSA 0x36 brute. PandwaRF has NO on-radio crypto break (all Kaiju cloud).
3. **Guided + RESUMABLE workflow** — capture→chip-ID→framed-SubBrute, resumable-across-power-off, no-desync clone, rolling analyzer, RollBack multi-slot, 76-entry vuln DB. PandwaRF has none of this.
4. **Standalone / untethered** — whole chain on-device w/ screen+keyboard. PandwaRF is BLE/USB-tethered to Android, per-command BLE latency.
5. **KeeLoq no-desync clone** — stores kdev+serial+counter, TX-synthesizes N/N+1 future frames; owner's fob keeps working. Offline once key known.
6. **Known-manufacturer-key KeeLoq clone works end-to-end offline TODAY** — exactly what PandwaRF gates behind account+internet+paid add-on.

## Ranked next builds (only 3 survived adversarial review)
1. **RollBack correctness fix (easy, ~4 tasks) — DO FIRST.** Real bug in a SHIPPING feature: engine hardcodes readiness at 2 slots but Mazda CVE-2022-36945 needs 3 (target-N lives only as free text); `subghz_rollback_ready()` (subghz_rollback.c:82-86) checks same-serial but NOT counter-adjacency. Fix: structured per-entry target-N in vehicle_vuln_db + strict consecutive-counter gate + vuln-browser→RollBack-capture menu link with N pre-set. Route STATIC-code Honda bugs (single replay) vs true-RollBack (consecutive) correctly. Host-testable via tests/test_subghz_rollback.c. Already #ifndef PUBLIC_BUILD.
2. **On-vehicle validation harness + evidence log (moderate, ~5 tasks).** Attacks the #1 weakness (ZERO OTA evidence). TWO honest pieces: (a) one-time host/CI encode→decode round-trip test per TX protocol (catches deterministic payload bugs like honda_v2 LOCK 0x82-vs-0xC2) — NOT a per-TX runtime gate (circular: encoder+decoder share TE tables). (b) private-gated on-device structured validation LOG (protocol/freq/preset/result/timestamp) to SD + review scene, so real OTA tests vs the OWNER's OWN vehicles bank durable evidence. Does NOT validate RMT/preset timing (needs scope/SDR — say so in-UI). #ifndef PUBLIC_BUILD (car material by association).
3. **Function-mask bit-granular brute + chip-ID auto-mask (moderate, ~5 tasks).** The ONE genuine feature PandwaRF has that fzcard lacks in general form: data=(cap AND mask) OR value at arbitrary BIT/symbol granularity (not byte-aligned), auto-seeding the fixed-bit mask from a capture + chip-ID so only unknown bits enumerate. Fixed-code only. Generalizes subbrute_protocol_create_candidate_for_existing_file() (subbrute_protocols.c:703, already byte-granular) + reuses tri-state 0/1/float LUTs (PT2262/Princeton/SMC5326). Realizes the deferred P2 "fixed-bit masking". Honest: marginal value = un-captured button/function codes + don't-care-bit receivers, NOT first access (a clean capture is already replayable). MUST stay #ifndef PUBLIC_BUILD + gc-verified out of webflasher (holding known serial fixed = days→seconds vs a gate the operator may not own).

## Sequence
1. RollBack bug fix first (real bug, shipping feature, cheap, host-testable — fix bugs before features).
2. Validation harness + evidence log second (highest strategic leverage — converts a won feature-race into field-proven trust; every OTA test banks evidence).
3. Function-mask brute third (only genuine gap-closer; built + validated under step-2 harness; mirror to src_cardputer_adv + gc-verify public).
4. STANDING: run the log against the owner's OWN vehicles/gates to accumulate first real RF evidence per protocol.

## Honest caveats (what we CANNOT do / dropped theater)
- **ZERO OTA evidence today** — every axis is host-verified or implemented-untested. Single-CC1101 async-TX timing (RMT underrun, CC1101 preset registers) is UNPROVEN; no pure-software check validates it (the encode→decode self-check is circular + radio-blind). Only a scope/SDR loopback or a real vehicle proves TX timing. **This is the single biggest weakness and PandwaRF's one durable edge (field-hardened TX).**
- **Front-end SNR is a HARDWARE ceiling** — VAG stalls 69/80 bits; Hyundai-TL sits UNDER the CC1101 noise floor. Bare module, no tuned matching/PA/LNA, no external-antenna SKU. No firmware closes this. "Auto preset/data-rate sweep on a stored capture" is RF-IMPOSSIBLE (a .sub is post-slicer pulse timing; bandwidth/deviation are analog, pre-slicer).
- **Dropped theater:** on-device known-key KeeLoq clone ALREADY fully ships offline (rebuilding as "offline Kaiju" beats nothing — real Kaiju solves the UNKNOWN-key case, still offline-PC-only); RollBack "wizard" ~90% ships; local auto-verdict analyzer ~90% ships (subghz_scene_analyze).
- **Physics-bound, out of single-CC1101 scope:** RollJam/jam-and-replay (2 radios), PKE relay (LF coil + 2 devices), Megamos/DST80/Hitag-2 immobilizer (125/134 kHz LF), BLE/UWB/NFC (out of band). On-device 48-bit Hitag-2 + unknown-key 64-bit KeeLoq recovery remain offline-PC/GPU only.
- A stored rolling-code capture cannot be known "live vs already-burned" from RF alone — don't badge rolling captures "replay/clone applicable" (operator false-confidence). Only fixed-code replay + known-key KeeLoq clone are reliably actionable from storage.

## Decisions for the owner
1. **Front-end hardware (the real cap):** commit to an external-antenna / PA-LNA SKU, or accept the SNR ceiling (VAG 69/80, Hyundai-TL under noise)? No firmware closes this — highest-impact decision, and it's hardware.
2. **Strategic spend:** (a) field-proven TRUST (validation harness + real OTA evidence) or (b) more features? Honest rec: **(a)** — the feature race is already won; the credibility gap is not.
3. **Authorized OTA targets:** which of your OWN vehicles/garage/gate remotes for banking first real RF evidence per protocol?
4. **Guardrail confirm:** mask brute (+ all fixed-code brute) stays #ifndef PUBLIC_BUILD + gc-verified out of public/webflasher.
