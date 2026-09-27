#!/usr/bin/env python3
"""
gen_icon_manifest.py -- list every built-in icon an asset pack can replace.

Parses components/assets/assets_icons.c and writes ASSET_PACK_ICONS.txt: one
line per icon, giving the exact NAME to use for the pack file, its pixel size,
and whether it is a static (.bmx) or animated (folder of frames) icon.

Run from the repo root:  python tools/asset_packs/gen_icon_manifest.py
"""
import re
from pathlib import Path

ASSETS_C = Path("components/assets/assets_icons.c")
OUT = Path("tools/asset_packs/ASSET_PACK_ICONS.txt")

PAT = re.compile(
    r"const Icon ([AI]_[A-Za-z0-9_]+) = \{\.width=(\d+),\.height=(\d+),\.frame_count=(\d+)")


def main() -> None:
    text = ASSETS_C.read_text(encoding="utf-8")
    rows = []
    for sym, w, h, fc in PAT.findall(text):
        name = sym[2:]  # drop the I_/A_ prefix -> the pack filename
        w, h, fc = int(w), int(h), int(fc)
        if fc > 1:
            rows.append((name, f"{name}/", f"{w}x{h}", f"animated, {fc} frames"))
        else:
            rows.append((name, f"{name}.bmx", f"{w}x{h}", "static"))
    rows.sort(key=lambda r: r[0].lower())

    lines = [
        "# Replaceable built-in icons for applab asset packs (themes).",
        "# Put files under  /asset_packs/<PackName>/Icons/  matching the FILE column.",
        "#   static   -> <FILE> is <name>.bmx  (a single PNG in make_asset_pack.py)",
        "#   animated -> <FILE> is <name>/     (a folder of frame_00.png, frame_01.png...)",
        f"# {len(rows)} icons total. Regenerate with tools/asset_packs/gen_icon_manifest.py",
        "#",
        f"# {'FILE':40} {'SIZE':>9}  KIND",
    ]
    for _name, file, size, kind in rows:
        lines.append(f"  {file:40} {size:>9}  {kind}")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {OUT}  ({len(rows)} icons)")


if __name__ == "__main__":
    main()
