# Sub-GHz Toolkit (Protocol Analysis + Keystore Export) — Design Spec

**Date:** 2026-07-10
**Status:** Approved (adapted from RocketGods-SubGHz-Toolkit; design confirmed via option).

## Goal

Port the **working** half of RocketGods-SubGHz-Toolkit into our Sub-GHz app as a
"SubGhz Toolkit" menu with two diagnostic/export actions:
1. **Protocol Analysis** — list every registered Sub-GHz protocol (name, type
   static/dynamic/unknown, flag) on screen + export to
   `/ext/subghz/analysis/protocol_analysis.txt` with a firmware-info header.
2. **Export Keystore** — dump the loaded plaintext KeeLoq manufacturer keystore
   (manufacturer, key hex, type) to `/ext/subghz/analysis/keeloq_keys.txt`.

## Context / what's adapted vs dropped

- RocketGods' original "Decrypt KeeLoq" walks the AES `keeloq_mfcodes` through the
  secure enclave. Our port's `furi_hal_crypto_decrypt` is a **stub** (no enclave),
  so decryption is impossible and would export garbage. **Dropped.**
- Instead, the "export keys" action dumps our **already-plaintext** keystore
  (the 68 keys shipped in `keeloq_mfcodes_user` + any user-added), which is
  genuinely useful and actually works.
- The Protocol Analysis half uses only the protocol registry (no crypto) — ports
  directly. Our port confirmed to have `subghz_protocol_registry_count()` /
  `subghz_protocol_registry_get_by_index()` and `SubGhzProtocol {name,type,flag}`.
- Delivered as **scenes in our existing subghz app** (a menu entry + a toolkit
  submenu), NOT a separate `.fap` — consistent with how we added Vehicle Vulns.

## UI

Entry point: a **"SubGhz Toolkit"** item in the Sub-GHz start menu
(`subghz_scene_start`), opening a submenu scene with two items:

1. **Protocol Analysis** → a scene that builds a scrollable text view (widget
   text-scroll) of every protocol: `<name>  <Static/Dynamic/Unknown>  flag=0x…`,
   AND writes the same (plus a firmware header) to
   `/ext/subghz/analysis/protocol_analysis.txt`; a small popup/footer notes the
   export path.
2. **Export Keystore** → iterates `subghz_keystore_get_data()` and writes each
   entry (`Manufacturer / Key(hex) / Type`) to
   `/ext/subghz/analysis/keeloq_keys.txt`; a popup shows success + the path (or an
   error popup on failure / empty keystore).

## Components

- New: `applications/main/subghz/scenes/subghz_scene_toolkit.c` (submenu: the two
  actions).
- New: `applications/main/subghz/scenes/subghz_scene_toolkit_protocols.c` (the
  protocol list view + export).
- Keystore export can live in the toolkit submenu scene's event handler (writes a
  file + popup) or a tiny helper — no separate view needed (popup only).
- Modify: `subghz_scene_config.h` (register scenes), `subghz_scene_start.c` (menu
  entry + route).
- Reuse: the protocol registry (find how our subghz app reaches it — via the
  `subghz_txrx` environment / the global `subghz_protocol_registry`), the keystore
  (`subghz_environment_get_keystore` → `subghz_keystore_get_data`), `storage`
  streams for file export, `widget`/`popup`/`submenu` modules.

## Data / file output

- `mkdir -p /ext/subghz/analysis`. Overwrite on each export (latest snapshot).
- Protocol file: firmware header (model/hw version/target from
  `furi_hal_version_*` — use the fields our port exposes), total count, then one
  block per protocol.
- Keystore file: header, total count, then one block per key.

## Non-Goals (YAGNI)

- No KeeLoq/AES decryption (impossible on this port).
- No separate `.fap`.
- No protocol editing / no live capture — read-only diagnostics/export.

## Testing

- **Compile gate:** `build_ci.bat`.
- **On-hardware checklist:** Sub-GHz → SubGhz Toolkit → Protocol Analysis lists
  the protocols + writes `protocol_analysis.txt`; Export Keystore writes
  `keeloq_keys.txt` with the 68 manufacturers; files present on SD; popups show
  the paths; nothing else in Sub-GHz regressed.

## Attribution

Adapted from RocketGods-SubGHz-Toolkit (GPL-3.0). Only the protocol-registry
walk + file-export structure are ported; the enclave-decrypt path is not used.

## Open Items (resolved during planning)

1. How our subghz app reaches the `SubGhzProtocolRegistry` (global extern vs via
   `subghz_txrx`/environment) — determine by reading `subghz_txrx.c` / how other
   scenes get the registry or environment.
2. Which `furi_hal_version_*` fields exist for the firmware header (our port has
   model_name/hw_version/hw_target; RocketGods' git-hash/build-date may not exist
   — use what's available).
3. Widget text-scroll for the protocol list (same as Vehicle Vulns) + popup module
   usage for the export confirmations.
