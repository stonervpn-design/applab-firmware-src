# Category-Folder Main Menu — Design

**Date:** 2026-07-11
**Status:** approved (brainstorming)

## Problem

The loader primary menu (`components/loader/loader_menu.c`) adds every menu-visible
app — `Sub-GHz` (the one internal `FlipperAppType.APP`) plus ~44 `MENUEXTERNAL`
apps — into one flat icon carousel sorted only by each app's `order=` field. With
~45 entries it is a long, undifferentiated scroll. Users want the apps grouped into
category folders (Sub-GHz, NFC, RFID, 2.4 GHz, WiFi, …).

## Goal

Replace the flat primary menu with a short list of **category folders**. Selecting a
category opens a sub-list of that category's apps; selecting an app launches it (same
launch path as today). `Settings` stays as the last primary entry, unchanged. No app
is ever lost: any menu-visible app whose id is not mapped falls into a **More** folder.

Main menu goes from ~45 items to 13 (12 categories + Settings).

## Categories (ordered) and membership

Membership is keyed by app **id** (falling back to display **name** if the loader's
app struct exposes no id). Apps not compiled in simply never appear; unmapped apps
go to **More**.

1. **Sub-GHz** — subghz, subghz_remote_ofw, radio_scanner, proto_pirate, weather_station
2. **2.4 GHz** — nrf24, fz_nrf24_jammer, esp_now
3. **LoRa / Mesh** — lora_monitor, meshtastic_sniffer
4. **NFC** — nfc, nfc_magic, mifare_fuzzer, rfid2_reader
5. **RFID / iButton** — lfrfid, ibutton
6. **WiFi** — wlan, defender, lan_attack, reverse_shell, wardriver, flippergotchi
7. **Bluetooth** — ble_spam, findmy
8. **Infrared** — infrared, flipper_xremote
9. **GPIO / Hardware** — gpio, wiegand, bus_tools
10. **USB / HID** — bad_usb, u2f
11. **Games** — tetris, snake20, asteroids, game15, texas_holdem, flipper_pong, blackjack, blackjack_split, doom, wolf3d, dice_app, tamagotchi_p1, t_embed_snake
12. **Tools** — clock, qrcode, mp3_player, key_copier, passport
13. **More** — catch-all for anything unmapped (only shown if non-empty)

Then **Settings** (unchanged).

## Architecture

- **`loader_menu_categories.{h,c}`** (new): pure, host-testable category model.
  - `LoaderCategoryId` enum + an ordered `LoaderCategoryDef { id, name, icon_hint }[]`.
  - Static `{ const char* appid; LoaderCategoryId cat; }` mapping table.
  - Pure fn `LoaderCategoryId loader_menu_category_for(const char* appid)` → the mapped
    category, or `LoaderCategoryMore` if not found. This is the one unit-tested piece.
- **`loader_menu.c`** (modified):
  - `loader_menu_build_menu`: instead of iterating apps flat, iterate the category list
    and add one primary `Menu` item per category **that has ≥1 compiled-in app**
    (compute counts by scanning `FLIPPER_APPS` + `FLIPPER_EXTERNAL_APPS` through
    `loader_menu_category_for`). Keep the `Settings` item last (unchanged).
  - New shared view `LoaderMenuViewCategory` (a `Submenu`), reused for every category:
    on category select, `submenu_reset`, set header to the category name, add its apps
    (both app lists, filtered by category, preserving `order` then name), switch to it.
  - Category-app select callback launches via the existing
    `loader_menu_start(name/path)` logic (reuse current internal/external callbacks).
  - Back from a category submenu → primary menu; Back from primary → exit (unchanged).
- Icons: reuse existing built-in `A_*_14` assets (no Pillow). Each category picks the
  most fitting available icon; fall back to `A_Plugins_14`. Exact icon per category is
  resolved at implementation time against `assets_icons.h`.

## Data flow

boot → loader_menu_thread → build primary Menu = [categories with ≥1 app] + Settings
→ user selects category → rebuild shared category Submenu with that category's apps
→ user selects app → loader_menu_start(app) (unchanged launch) → Back → primary.

## Non-goals / YAGNI

- No `category=` field added to `application.fam` and no changes to the fam build
  tooling (known-fragile; a static map in the loader is lower risk and keeps the app
  list resolvable at unmapped→More). New apps land in **More** until mapped — acceptable.
- No favorites row, no per-category icons authored from PNG, no search.
- Settings submenu behavior unchanged.

## Testing

- **Host test** (`tests/test_loader_categories.c`): compile `loader_menu_categories.c`
  pure and assert `loader_menu_category_for()` returns the expected category for a
  representative appid per bucket, and `LoaderCategoryMore` for an unknown id.
- **Firmware build** green (EXIT=0, `=== BUILD OK ===`).
- **On-device**: flash, confirm main menu shows the category folders, each opens its
  apps, launching works, Back navigates correctly. (Manual; recon-style visual check.)

## Risk

Low. Single subsystem (loader menu); launch path reused unchanged; catch-all prevents
lost apps; the only new logic (the id→category map) is pure and unit-tested.
