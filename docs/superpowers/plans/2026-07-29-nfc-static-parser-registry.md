# NFC Static Supported-Card Parser Registry — Implementation Plan (Phase 1)

> REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** Make the supported-card parsers work on **no-PSRAM** boards (Cardputer-ADV) by compiling them into the firmware and resolving them through a static registry, instead of the runtime `.fal` ELF loader (which is skipped entirely when there's no PSRAM — `nfc_supported_cards.c:189`). Net effect: 0 → 37 parsers working on the Cardputer, and adding more becomes a compile-in one-liner.

**Architecture:** Each parser already exports `const FlipperAppPluginDescriptor* <name>_plugin_ep(void)` whose `->entry_point` is the `NfcSupportedCardsPlugin*`. Statically we call that directly (validating `appid=="NfcSupportedCardPlugin"`, `ep_api_version==1`), mirroring the loader at `nfc_supported_cards.c:127-135`. Compile the parser `.c` into the `nfc` app object lib; a generated registry maps name→ep. The loader uses the static registry instead of scanning `*_parser.fal`.

**Tech Stack:** C, ESP-IDF, fam build (`application.fam` sources → `generate.py` → OBJECT libs).

## Global Constraints
- Must work on BOTH boards. Simplest: use the static registry ALWAYS (drops the `/ext` `.fal` dependency for parsers). PSRAM boards lose nothing.
- Do NOT modify parser `.c` logic — only compile them in + resolve statically.
- Build private via `build_only_private.bat` (m5stick) / `build_adv_only_private.bat` (cardputer); verify `SENTINEL_OK`, 0 `error:`, fresh bin.
- Card-level verification is the tester's; the build-level win (parsers load on cardputer) is checked via boot log `Loaded N plugins` / no "No PSRAM: skipping".

## Tasks

- [ ] **Task 1 — Generate registry + compile parsers in (the build de-risk).**
  - Create `helpers/nfc_supported_cards_static.h` (count / get_by_index(name_out) / get_by_name).
  - Generate `helpers/nfc_supported_cards_static.c` from `plugins/supported_cards/*.c` via `tools/gen_nfc_static_parsers.py` (extract each `_plugin_ep`, name = file basename). Externs + `{name, ep}` table + `resolve()` that validates appid/api_version.
  - Build inclusion: add the parser sources to the `nfc` app in `application.fam` so they link into the binary (they're currently under `!plugins`). Keep the PLUGIN `.fal` decls for now (separate link unit, no conflict).
  - Wire ONE call site so the linker keeps it (Task 2 does the real swap).
  - Build m5stick private → confirm ALL parsers compile in (this is where a parser with a missing in-binary dep would surface). Fix any that don't.

- [ ] **Task 2 — Swap the loader to the static registry.**
  - `nfc_supported_cards.c`: `load_cache` iterates `nfc_supported_cards_static_*` (build the same `plugins_cache_arr`); `get_plugin(name)` (parse/verify paths ~lines 257/305) resolves via `get_by_name`. Remove the no-PSRAM skip (static needs no PSRAM). Keep the `.fal` path only as a `#else`/fallback if desired, else delete.
  - Build m5stick private → parsers still enumerate.

- [ ] **Task 3 — Build + verify both boards.**
  - Build m5stick + cardputer private. Cardputer boot log must show `Loaded N plugins` (NOT "No PSRAM: skipping"). Rebuild public variants too.
  - Tester: on the Cardputer, read a card a parser recognizes (e.g., a transit card) → the card-specific label now appears (was impossible before).

- [ ] **Task 4 (Phase 1b) — Extend + Topaz.** Add community parsers (drop `.c` + regenerate registry). Topaz/Jewel Type-1 as a separate protocol add.

## Notes
- Generator keeps the registry regenerable — adding a parser = drop `.c`, rerun `tools/gen_nfc_static_parsers.py`, rebuild.
- If a parser fails to compile in-binary (in-`.fal` it had its own libs), either add the lib to the nfc app or exclude that parser from the static set and log it — never silently drop coverage.
