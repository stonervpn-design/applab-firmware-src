#!/usr/bin/env python3
"""Repack the M5Stick S3 public web-flasher zip.

- Replaces the merged bin entry with the freshly built one on disk.
- Injects sd_content/Manifest (Flipper resource manifest) computed from the
  zip's own sd_content/* files, so the reference SD copy also has the Manifest
  that stops the desktop "No Databases" (L0_NoDb) screen. The flashed image
  already carries its own Manifest baked in; this only fixes the reference copy.
"""
import hashlib
import io
import os
import sys
import time
import zipfile

ZIP = sys.argv[1]
BIN = sys.argv[2]  # path to freshly built merged bin on disk
BIN_ENTRY = os.path.basename(BIN)
SD_PREFIX = "sd_content/"

with zipfile.ZipFile(ZIP) as z:
    items = {i.filename: z.read(i.filename) for i in z.infolist() if not i.is_dir()}
    dirs = [i.filename for i in z.infolist() if i.is_dir()]

# Swap in the new bin.
with open(BIN, "rb") as f:
    items[BIN_ENTRY] = f.read()

# Build sd_content/Manifest from the sd_content payload files.
sd_files = sorted(
    p for p in items
    if p.startswith(SD_PREFIX) and p != SD_PREFIX + "Manifest" and p != SD_PREFIX + "README.txt"
)
sd_dirs = sorted({os.path.dirname(p) for p in sd_files})
lines = ["V:1", "T:%d" % int(time.time())]
emitted = set()
for p in sd_files:
    rel_dir = os.path.dirname(p)[len(SD_PREFIX):]
    # emit each ancestor dir once, in order
    parts = rel_dir.split("/") if rel_dir else []
    acc = ""
    for part in parts:
        acc = (acc + "/" + part) if acc else part
        if acc not in emitted:
            lines.append("D:%s" % acc)
            emitted.add(acc)
    rel = p[len(SD_PREFIX):]
    data = items[p]
    lines.append("F:%s:%d:%s" % (hashlib.md5(data).hexdigest(), len(data), rel))
items[SD_PREFIX + "Manifest"] = ("\n".join(lines) + "\n").encode()

# Rewrite zip deterministically.
tmp = ZIP + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
    for d in dirs:
        z.writestr(d, b"")
    for name in sorted(items):
        z.writestr(name, items[name])
os.replace(tmp, ZIP)
sys.stderr.write("repacked %s: bin=%d bytes, sd_content/Manifest=%d files\n"
                 % (ZIP, len(items[BIN_ENTRY]), len(sd_files)))
