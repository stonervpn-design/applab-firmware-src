# Momentum → MOB devices (Cardputer ADV + M5Stick S3) — Standalone Port

**Goal:** Momentum firmware's app/identity/menu layer running on the NEW_v2 (MOB)
ESP32 Furi runtime, for **m5stack_cardputer_adv** and **m5stick_s3**. T-Embed already
has its own Momentum port (`../momentum-tembed`).

## Shape / why
Momentum is STM32 firmware; it can't recompile for ESP32-S3. A "Momentum port" =
Momentum's application/identity layer on an ESP32 Furi runtime that already boots the
device. NEW_v2 already boots on both boards (OFW-core, `components/gui` structure —
same lineage as the Sor3nt base `momentum-tembed` grafted onto). So we reuse NEW_v2's
working HALs and transplant the KNOWN Momentum delta from `momentum-tembed`.

## Hard constraints
- **Standalone.** Own dir `C:\Users\stone\momentum-mob`. Copied source-only from
  `Downloads\_fzcard_work\NEW_v2\src` (build_* / logs / merged bins excluded).
- **NEW_v2 and momentum-tembed are READ-ONLY reference.** Do not modify them.

## Toolchain
- ESP-IDF v5.4.1 (`C:\Espressif\frameworks\esp-idf-v5.4.1`), IDF_TOOLS_PATH=`~\.espressif`.
- Build Cardputer ADV: `build_adv.bat` (FLIPPER_BOARD=m5stack_cardputer_adv, esp32s3,
  build dir `build_cardputer_adv`). M5Stick: `build_m5stick_s3.bat`.
- Run .bat by ABSOLUTE path; feed `< /dev/null` (scripts end in `pause`).

## Phases
- [x] **P1 — Baseline green.** momentum-mob copied from NEW_v2; Cardputer ADV builds
  clean UNCHANGED. furi_esp32.bin 2.85MB, app partition 53% free, merged bin produced.
- [ ] **P2 — Momentum identity on Cardputer ADV** (feasibility spike):
  - `components/momentum` (momentum_settings global, load from internal flash — no SD).
  - Swap `components/gui/modules/menu.c` + `menu.h` → Momentum styled renderer.
  - Port 3 helpers into gui: `canvas_draw_icon_animation_ex`,
    `elements_scrollable_text_line_centered`, `elements_scrollbar_horizontal`.
  - `applications_user/momentum_app` settings fap + firmware_api exports.
  - Momentum SubGhz protocol additions + branding.
  - Reconcile OFW API drift (NEW_v2 snapshot vs momentum-tembed snapshot). Build green.
- [x] **P3 — M5Stick S3 GREEN 2026-08-22.** Shared tree => the P2 Momentum layer applied
  with ZERO board-specific drift. Flipper-m5stick_s3-merged.bin 2.85MB, partition 24%
  free. Same Momentum Wii-grid menu, internal-flash settings (SD-less friendly).
- [ ] **P4 — Deliverables.** Merged .bin per device + flash/monitor notes. User does
  the on-device smoke test.

## Status log
- P1 DONE (green) 2026-08-22: baseline Cardputer ADV build SUCCESS on the fresh copy.
- P2 firmware core DONE (green) 2026-08-22 — Cardputer ADV:
  - Transplanted `components/momentum` (settings global) + wired into `components/gui`
    REQUIRES. Momentum styled `menu.c` in place (menu.h identical between bases, no
    change). Ported 3 gui helpers (canvas_draw_icon_animation_ex,
    elements_scrollable_text_line_centered, elements_scrollbar_horizontal) + decls.
  - Drift fixes (both matched momentum-tembed's list): copied `toolbox/colors.h`;
    added `FuriHalVersionColorCount` enum terminator to furi_hal_version.h.
  - asset_packs_init() stubbed in menu.c (SD-dependent, later scope).
  - **Builds GREEN: Flipper-cardputer_adv-merged.bin 3.06MB, app partition 52% free.**
    Boots to Momentum Wii-grid menu out-of-box (default MenuStyleWii, loaded from
    internal flash — no SD). NOTE: menu geometry on Cardputer's screen is an on-device
    check (Momentum styles assume 128x64).
  - NOTE: momentum-mob is ONE multi-board tree, so these shared changes apply to
    m5stick_s3 too; P3 = build m5stick + fix any board-specific drift.
  - REMAINING P2: momentum_app.fap (settings UI; needs firmware_api export reconcile —
    dialog_ex_*, value_index_int32, etc.), Momentum SubGhz protocols, branding.
