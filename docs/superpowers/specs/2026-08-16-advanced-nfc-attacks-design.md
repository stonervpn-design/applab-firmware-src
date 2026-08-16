# Advanced NFC Attack Suite — Design

**Date:** 2026-08-16
**Status:** Approved for implementation planning
**Scope:** PRIVATE-only (`#ifndef PUBLIC_BUILD`). Superset `src_m5stick_s3` first, then mirror to `src_cardputer_adv` and `src_lilygo_t_embed_cc1101`.
**Author:** paired session
**Revision:** 2026-08-16 rev2 — expanded to "all feasible" advanced-NFC attacks (added DESFire config audit, Plus SL3 keyed auth, MFClassic backdoor/static-nonce dump, FeliCa keyed dump, SLIX password dictionary, experimental NTAG tear-off).

## Goal

Add real, on-device attack tooling for every "advanced" NFC type the port already *reads* but cannot *attack*: MIFARE DESFire (EV0/EV1/EV2/EV3), MIFARE Plus (SL1 + SL3 keyed), MIFARE Classic hardened/backdoor variants, Ultralight/NTAG21x, FeliCa, and ISO15693 SLIX/SLIX2.

## Honest feasibility framing (non-negotiable, drives scope)

The advanced types have **no crypto1-style key recovery**. DESFire EV1+/Plus SL3 use real DES/3DES/AES; there is no practical on-device key crack. What is real and shippable:

- **DESFire:** authenticate with *known / default / dictionary* keys, then dump every file that key authorizes. This is "auth-with-known-keys + dump," not "crack the card."
- **Plus SL1:** SL1 backward-compat exposes crypto1 sectors — a **genuine** key crack via the existing MFClassic machinery.
- **Ultralight/NTAG21x:** **dictionary + UID-derived-password generators**, not brute force. A full 32-bit `PWD_AUTH` brute is ~4×10⁹ round-trips ≈ years on-device and is explicitly **out of scope** (labeled infeasible in-UI, like the Hitag-2 note).

## Existing building blocks (verified in tree)

- `components/nfc/protocols/mf_desfire/` — full read-only poller: `read_version/free_memory/key_settings/key_version(s)/application_ids/select_application/read_file_ids/read_file_settings` + raw `mf_desfire_poller_send_chunks` (APDU transport). **No authenticate primitive today.**
- mbedtls DES3 + AES already linked and used (`components/nfc/protocols/felica/felica.c` → `mbedtls_des3_*`). Crypto provider for DESFire auth — no new dependency.
- `components/nfc/helpers/crypto1.{c,h}` — crypto1 for MFClassic/Plus SL1.
- `components/nfc/protocols/mf_plus/` — full poller with `security_level` field (SL1/SL3 detectable).
- MFClassic dict + nested attack, MFKey nonce collection, Ultralight-C dict attack, Ultralight `capture_pass`/`unlock`, DES-auth key input — existing attack scenes/patterns to clone.
- nfc_magic (gen1a/gen2/gen4) for magic-card writing (not modified here).

## Component 1 — DESFire default-key auth + auto-dump (bulk of work)

### 1a. Crypto module — `components/nfc/protocols/mf_desfire/mf_desfire_crypto.{c,h}`
Implements the DESFire authentication state machine on top of `send_chunks`:

- **Legacy authenticate `0x0A`** — DES / 2K3DES / 3K3DES. Card returns `E(RndB)`; decrypt, rotate-left, encrypt `RndA || RndB'`, send, verify `RndA'`.
- **ISO authenticate `0x1A`** — 16-byte challenges, DES/3DES.
- **AES authenticate `0xAA`** — AES-128, 16-byte challenges.
- **Session-key derivation** per key type (DES/2K3DES/3K3DES/AES rules).
- **Comm-mode helpers**: plain, MAC (CMAC for EV1+, legacy CRC-MAC for EV0), and encrypted (CBC) — see phasing.
- Uses `mbedtls_des3_*` and `mbedtls_aes_*`. IV/CBC state kept in an auth context struct.

### 1b. Poller extension — `mf_desfire_poller.{c,h}` (+ `_i`)
- `MfDesfireError mf_desfire_poller_authenticate(MfDesfirePoller*, uint8_t key_no, MfDesfireKeyType, const uint8_t* key);`
- `MfDesfireError mf_desfire_poller_read_file_data_authed(...);` — reads honoring the file's comm mode using the live session.

### 1c. Attack engine — `applications/main/nfc/helpers/mf_desfire_attack.{c,h}`
- **Key dictionary**: built-in defaults per type (16×`00` DES, 24×`00` 3K3DES, 16×`00` AES) + optional SD dictionary `nfc/assets/mf_desfire_dict.nfc` (lines: `<keytype>:<hex>`).
- **Sweep**: PICC master key (AID 000000) + every enumerated AID × every key slot × every dictionary entry of a compatible type. Stop-per-slot on first success.
- **Record**: list of `(aid, key_no, key_type, key)` that authenticated.
- **Dump**: for each authed context, read all readable files → merge into the `MfDesfireData` model so `more_info`/save reflect recovered content.
- Cooperative/yielding loop with progress callback (count done / total, current AID) and cancel.

### 1d. Scenes / UI
- Add **"Unlock / Key Attack"** action to the DESFire actions menu (`nfc_scene_mf_desfire_app.c` / extra actions).
- `nfc_scene_mf_desfire_dict_attack.c` — progress view cloned from `nfc_scene_mf_classic_dict_attack.c` (dict progress widget). Shows keys tried, keys found, files dumped.
- On completion → results into existing `more_info`; recovered keys listed and persisted with the saved device.

### 1e. Phasing (de-risk the crypto)
- **Phase A (ship first):** auth (`0x0A`/`0x1A`/`0xAA`) + **plain and MAC** comm-mode file read. Proves keys, covers non-encrypted files/cards. HW-proven before Phase B.
- **Phase B:** full **encrypted-comm** (CBC) file read — transit cards. Highest risk: CRC placement + IV chaining differ EV0 vs EV1/EV2/EV3.

### 1f. DESFire config-weakness audit (no auth required)
Passive report generated during read / on demand:
- Files with **free-read** access (comm mode + access rights from `read_file_settings`).
- **Comm mode** per file (plain / MAC / encrypted).
- **Default master key still set** heuristic (key version `0x00` + successful default auth from 1c).
- **Random UID** enabled/disabled.
- **Key settings**: master-key changeable, dir-listing without auth, config changeable, max keys.
- Rendered into `more_info`; saved with device.

## Component 2 — MIFARE Plus attacks

### 2a. SL1 crypto1 crack (low effort)
- Detect `mf_plus ... security_level == SL1`.
- Add **"Attack as MIFARE Classic (SL1)"** action that routes the SL1 backward-compat card through the **existing** MFClassic dict + nested crypto1 attack. Wiring, not new crypto.
- If HW testing shows an SL1 Plus card does not answer classic `AUTH` in this frontend, downgrade to "detect + report SL1" and note it (do not fake a crack).

### 2b. SL3 default/known-AES-key auth + dump (feasible, medium effort)
- `mf_plus_poller` currently exposes only `read_version`; implement an **AES authenticate primitive** on the Plus poller, **reusing the AES code from `mf_desfire_crypto` (Component 1a)**.
- Dictionary of default/known Plus AES keys (`FFFF…`, transit) → authenticate sectors → dump readable sectors. Same dictionary/engine shape as DESFire (1c). Scheduled after DESFire so AES code is proven first.
- No SL3 *key recovery* — known/default keys only.

## Component 3 — MIFARE Classic backdoor / static-nonce dump (low effort — capability already in poller)

- The poller already ships backdoor keys: Fudan FM11RF08S `A396EFA4E24F` (static-encrypted, `MfClassicBackdoorAuth3`), `A31667A8CEC1` (Fudan/Infineon/NXP, Auth1), `518B3354E760` (Fudan, Auth2).
- Add a one-tap **"Backdoor dump"** action: detect a backdoor-vulnerable card, authenticate via the backdoor command, recover per-sector keys via the static-encrypted-nonce path, and dump.
- Surface + verify the existing capability in the dict-attack UI; confirm on real FM11RF08S during HW test.
- **Hardnested is excluded** (unimplemented TODO FL-3926, RAM/CPU-heavy, would break no-PSRAM Cardputer parity).

## Component 4 — FeliCa keyed service read/dump (feasible)

- `felica_poller` has `read_blocks` + existing DES `felica_auth`.
- Dictionary of known FeliCa card keys → `felica_auth` → read protected services/blocks → dump into model. Reuses the existing DES auth path and the dict-engine pattern.

## Component 5 — ISO15693 SLIX / SLIX2 password dictionary (feasible)

- `slix` already models Read / Write / Privacy / EAS / Destroy password slots with default values; `iso15693_3_poller_send_frame` sends the custom `GET_RANDOM_NUMBER` + `SET_PASSWORD` sequence.
- Dictionary of default + known SLIX passwords → attempt Read/Privacy/EAS unlock; on success, disable privacy mode and read blocks. Progress scene reused from UL-C dict pattern.
- Full 32-bit password brute is out of scope (dictionary only).

## Component 6 — Ultralight/NTAG21x tear-off (EXPERIMENTAL, optional)

- Timing-based tear-off to reset OTP / one-way counters on vulnerable UL/NTAG tags (interrupt a write mid-cycle).
- Marked **EXPERIMENTAL** behind a warning scene; success is tag- and timing-dependent and may brick the tag. Include only if HW timing on the ESP32 frontend proves controllable; otherwise ship disabled with a note.

## Component 7 — Ultralight / NTAG21x password recovery (honest scope)

- **In scope:** password **dictionary** (`nfc/assets/mf_ul_pwd_dict.nfc` + built-in `FFFFFFFF`, `00000000`, common transit/hotel defaults) **+ UID-derived generators** for published algorithms (Amiibo-style, common transit derivations). Each candidate → `PWD_AUTH (0x1B)`, capture `PACK` on success.
- Extends the existing `capture_pass`/`unlock` flow with a "Password dictionary attack" scene reusing the Ultralight-C dict-attack progress pattern.
- **Out of scope (labeled in-UI):** exhaustive 32-bit brute — infeasible on-device.

## Cross-cutting

### Verification
- **Host crypto harness** `tools/desfire_auth_host_test/`: compile `mf_desfire_crypto.c` (+ a thin mbedtls shim or system mbedtls) on host; assert against **published DESFire DES/2K3DES/AES auth test vectors**. Must pass before any flash. Mirrors the ProtoPirate host-decode harness pattern.
- **On-device:** build all three trees → flash M5Stick S3 (COM10) / Cardputer ADV (COM12) / T-Embed CC1101+ (COM11) → HW-test real DESFire + Plus cards with serial logging. User has cards.

### Gating & parity
- All new code PRIVATE-only via `#ifndef PUBLIC_BUILD` (menu actions, scenes, dictionaries). Public build must not expose any of these attacks.
- Implement in `src_m5stick_s3` (superset), then mirror byte-for-byte to `src_cardputer_adv` and `src_lilygo_t_embed_cc1101`. No feature stripped from any private tree (no-strip rule).

## Non-goals (documented infeasible / excluded)
- **DESFire / Plus SL3 key recovery** (side-channel / DFA) — infeasible on-device. Known/default/dictionary keys only.
- **Exhaustive 32-bit brute** for Ultralight/NTAG/SLIX passwords — dictionary + UID-derivation only.
- **MFClassic hardnested** — unimplemented TODO, RAM/CPU-heavy, breaks no-PSRAM Cardputer parity. Nested + backdoor cover realistic cards.
- **Relay / downgrade** multi-device attacks — require two coordinated radios.
- No changes to nfc_magic, emulation, or the public build's NFC surface.
- No new NFC frontend/HAL work (assumes current ST25R3916 / board reader handles ISO-DEP APDU exchange; validated during HW test).

## Build order
1. `mf_desfire_crypto` + host vector harness (Phase A crypto: auth + plain/MAC).
2. DESFire poller `authenticate` + `read_file_data_authed`.
3. `mf_desfire_attack` engine + dictionary loader.
4. DESFire dict-attack scene + config-audit (1f) + menu wiring; build + flash + HW-test Phase A.
5. MFClassic backdoor / static-nonce dump action (C3) — surface existing poller keys; HW-test on FM11RF08S.
6. Ultralight/NTAG21x password dictionary attack (C7, independent/parallelizable).
7. Plus SL1 detection + route to MFClassic attack (C2a).
8. FeliCa keyed dump (C4) + SLIX password dictionary (C5) — independent, share dict-engine.
9. DESFire Phase B (encrypted-comm read) after Phase A is HW-proven.
10. Plus SL3 keyed-AES auth + dump (C2b), reusing proven DESFire AES code.
11. NTAG tear-off (C6) — only if HW timing proves controllable; else ship disabled.
12. Mirror all to cardputer_adv + t_embed; build all three.
