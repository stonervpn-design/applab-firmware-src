# QR Code Generator App — Design Spec

**Date:** 2026-07-10
**Status:** Approved (design confirmed). Reuses upstream `flipperzero-qrcode` encoder+render.

## Goal

A standalone **QR Code** app (main menu) that generates and displays QR codes on
the 128×64 canvas from four content sources: **Text/URL**, **WiFi join**,
**From File**, **Contact (vCard)**. Generation only (the Cardputer has no camera).

## Approach: port the proven encoder+render, add the input flows

Upstream ElicoftZ 1.16 bundles **bmatcuk/flipperzero-qrcode** (MIT) at
`applications_user/flipperzero-qrcode/`:
- `qrcode.c` / `qrcode.h` — Richard Moore's QR encoder. API:
  `qrcode_getBufferSize(version)`, `qrcode_initBytes(qr, modules, mode, version,
  ecc, data, len)` (modes NUMERIC/ALPHANUMERIC/BYTE; ecc LOW/MEDIUM/QUARTILE/HIGH),
  `qrcode_getModule(qr, x, y)`.
- `qrcode_app.c` — render logic (scaled module draw via `canvas_draw_box`/`dot`,
  auto pixel-size to fit, ECC/version handling, "Message is too long" fallback).

We **port `qrcode.c/h` verbatim** (proven, MIT) and **adapt the render logic**,
then build a NEW app around them with a menu + the four input flows (upstream's
app is file-load-centric; we want interactive entry). This reuses the hard,
proven parts and delivers the requested menu.

## App structure

`applications/main/qrcode/` (compiled-in app, main-menu entry "QR Code"):
- `lib/qrcode.{c,h}` — vendored MIT encoder (verbatim from upstream).
- App + view-dispatcher/scene-manager, scenes below, one QR **display view**.

### Main menu (start scene)
`Text / URL` · `WiFi` · `From File` · `Contact (vCard)`.

### Flows
- **Text/URL:** `text_input` → the entered string → QR view.
- **WiFi:** three inputs — SSID (`text_input`), Password (`text_input`), Auth
  (a small selector WPA/WEP/Open) → build `WIFI:S:<ssid>;T:<WPA|WEP|nopass>;P:<pass>;;`
  (escape `\ ; , : "` per the WiFi-QR spec) → QR view.
- **From File:** `dialog_file_browser` over `/ext` → read the file contents
  (whole file, or first line if huge) → QR view. (Also accepts upstream `.qrcode`
  files' `Message:` line if present, else raw text.)
- **vCard:** three inputs — Name, Phone, Email → build
  `BEGIN:VCARD\nVERSION:3.0\nN:<name>\nTEL:<phone>\nEMAIL:<email>\nEND:VCARD` → QR view.

### QR display view (ported/adapted from qrcode_app.c render)
Encode the payload (BYTE mode; ECC default MEDIUM; auto-pick the smallest version
that both encodes AND fits the canvas). Draw the module matrix black-on-white,
scaled by an integer `pixel_size` computed to fit within the 128×64 canvas with a
quiet-zone margin, centered. If the payload can't fit a scannable version on the
screen, show **"Too long for screen QR"** instead of an unscannable blur (reuse
upstream's too-long handling). Back returns to the menu.

## Constraints

- 128×64 canvas (aspect-scaled onto the 240×135 panel, so modules render ~2×
  physically → scannable). Practical payload limit ≈ a URL / short WiFi creds
  (~version 3–4, ≈50–80 bytes at MEDIUM ECC). Over-limit → the "too long" screen.
- BYTE mode for all (handles URLs/WiFi/vCard/arbitrary text).

## Components

- `applications/main/qrcode/application.fam` (main-menu app, `requires: gui,
  dialogs`).
- `applications/main/qrcode/lib/qrcode.{c,h}` (vendored MIT encoder).
- `qrcode_app.{c,h}` + scenes (`scene_start`, `scene_text`, `scene_wifi`,
  `scene_vcard`, `scene_file`, `scene_display`) + the QR display view.
- Reuse: `text_input`, `dialog_file_browser` (dialogs), `submenu`, `variable_item_list`
  (for the auth selector), canvas draw.

## Non-Goals (YAGNI)

- No QR scanning (no camera).
- No saving generated QR as image (display only; content can already be saved as
  a .qrcode/.txt by the user).
- No batch/multi-QR from a multi-line file (render one QR; use first line if huge).

## Testing

- **Compile gate:** `build_ci.bat`.
- **On-hardware checklist:** QR Code app → Text/URL: type a URL → scannable QR
  (phone opens it); WiFi: enter SSID/pass/WPA → phone scans → offers to join;
  From File: pick a .txt → renders its content; vCard: enter name/phone/email →
  phone scans → add-contact; over-long text → "Too long for screen QR"; Back
  navigation clean.

## Attribution

`qrcode.c/h` is MIT (Richard Moore / bmatcuk's flipperzero-qrcode). Encoder
vendored verbatim; the app/menu/input flows are new.

## Open Items (resolved during planning)

1. Exact `text_input` + `dialog_file_browser` usage — read a sibling app that
   uses them (e.g. bad_usb / archive) during planning.
2. How compiled-in apps get a main-menu entry (application.fam + the loader menu)
   — mirror an existing main app (clock_app).
3. Auto version/pixel_size fit algorithm — port from upstream `qrcode_app.c`.
