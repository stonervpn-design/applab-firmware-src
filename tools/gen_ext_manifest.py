#!/usr/bin/env python3
"""Generate a Flipper resource /ext/Manifest for a staged /ext tree.

The desktop AnimationManager shows the built-in "L0_NoDb" ("No Databases")
blocking animation whenever /ext/Manifest is absent (see
animation_manager.c: storage_file_exists(EXT_PATH("Manifest"))). Our internal-
flash /ext image only staged infrared/nfc/subghz assets and never had a
top-level Manifest, so the stick booted straight into the "no databases" screen.

Format (matches update_util/resources/manifest.c):
  V:<version>
  T:<unix_timestamp>
  D:<dir_path_relative_to_ext>
  F:<md5_hex>:<size_bytes>:<file_path_relative_to_ext>
Paths use forward slashes. Directories are emitted before the files they contain.
"""
import hashlib
import os
import sys
import time


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: gen_ext_manifest.py <stage_dir>\n")
        return 2
    stage = os.path.abspath(sys.argv[1])
    if not os.path.isdir(stage):
        sys.stderr.write("not a directory: %s\n" % stage)
        return 2

    lines = ["V:1", "T:%d" % int(time.time())]
    # Deterministic, top-down walk so a directory precedes its contents.
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        files.sort()
        rel_root = os.path.relpath(root, stage).replace(os.sep, "/")
        if rel_root != ".":
            lines.append("D:%s" % rel_root)
        for name in files:
            if name == "Manifest":
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, stage).replace(os.sep, "/")
            lines.append("F:%s:%d:%s" % (md5_of(full), os.path.getsize(full), rel))

    out = os.path.join(stage, "Manifest")
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    sys.stderr.write("wrote %s (%d entries)\n" % (out, len(lines) - 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
