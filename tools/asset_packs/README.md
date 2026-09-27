# Asset packs (custom themes) for the applab ESP32 Flipper ports

An **asset pack** replaces the firmware's built-in **icons** and **fonts** with
your own, loaded from the microSD at runtime. It's the "themes" system for the
Momentum flavor on the Cardputer ADV, M5Stick S3 and LilyGo T-Embed CC1101+.

> microSD only. The Cardputer and T-Embed read packs from the card. The M5Stick
> S3 has no card slot, so it uses the built-in look only.

## Install a pack

1. Copy the pack folder to the root of a FAT32 microSD:

   ```
   /asset_packs/<PackName>/
   ```

2. Insert the card and, on the device, go to
   **Settings → Desktop → Asset Pack** and pick your pack.
   The change applies immediately — no reboot. Pick **Default (built-in)** to
   turn it back off.

## What's in a pack

```
/asset_packs/<PackName>/
  Icons/
    <name>.bmx                 a static icon
    <name>/                    an animated icon...
      meta
      frame_00.bm
      frame_01.bm
      ...
  Fonts/
    Primary.u8f                optional font overrides
    Secondary.u8f              (slots: Primary, Secondary, Keyboard,
    Keyboard.u8f                BigNumbers, BatteryPercent)
    BigNumbers.u8f
    BatteryPercent.u8f
```

- You only include the icons/fonts you want to change; everything else stays
  built-in.
- **`<name>`** must exactly match a built-in icon name. The full list of names,
  sizes, and which are static vs animated is in **`ASSET_PACK_ICONS.txt`**
  (276 icons). Your replacement can be any size — the UI uses your dimensions.
- Fonts must already be in u8g2 `.u8f` format.

## Make a pack from PNG images

Use `make_asset_pack.py` (needs `pip install pillow`). Lay your images out to
mirror the pack, using the icon names from `ASSET_PACK_ICONS.txt`:

```
mytheme_src/
  Icons/
    Battery_26x8.png           -> static  (one PNG)
    Alarm_47x39/               -> animated (a folder of frames)
      frame_00.png
      frame_01.png
      meta.txt                 -> optional: one line "frame_rate=2"
  Fonts/
    Primary.u8f                -> copied through as-is
```

Then build:

```
python make_asset_pack.py mytheme_src "My Theme" --out build
# -> build/My Theme/  ... copy that folder to /asset_packs/ on the SD
```

**Pixel rule:** a pixel is drawn (in the UI foreground colour) when it is dark
(luminance < 128) and not transparent. White or transparent pixels are the
background. So author icons as black-on-transparent (or black-on-white).

## Worked example

`example_src/Icons/` holds two source PNGs (a custom battery and lock icon).
Building them produces the ready-to-use pack in `build/Example/`:

```
python make_asset_pack.py example_src "Example" --out build
```

Copy `build/Example` to `/asset_packs/` on the card and select it to see the
battery and lock-screen icons change.

## Icons vs animations vs colours

- **Icons & fonts** — this system (asset packs).
- **Idle/desktop animations** — the dolphin animations are separate and already
  load from **`/dolphin/manifest.txt`** on the SD (the standard Flipper custom
  animations mechanism). Drop custom animation sets there.
- **UI colours** — the two-colour UI palette (background/foreground, plus LED
  and backlight) is set on-device under **Settings → Notifications** and is
  independent of asset packs.

## File format (for tooling authors)

All little-endian. A *frame blob* is `0x00` (uncompressed flag) followed by XBM
bitmap data: 1 bit per pixel, LSB-first within each byte, each row padded to a
whole number of bytes. This is what the firmware's `compress_icon_decode()`
reads.

| File | Layout |
|------|--------|
| `Icons/<name>.bmx` | `int32 width`, `int32 height`, then one frame blob |
| `Icons/<name>/meta` | `int32 width`, `int32 height`, `int32 frame_rate`, `int32 frame_count` |
| `Icons/<name>/frame_NN.bm` | one frame blob (dimensions come from `meta`) |
| `Fonts/<Slot>.u8f` | a raw u8g2 font |

Regenerate `ASSET_PACK_ICONS.txt` after icons change:
`python gen_icon_manifest.py` (from the repo root).
