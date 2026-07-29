#!/usr/bin/env python3
"""Fail (exit 1) unless a partition image contains the M5Stick 'storage' partition.

Guards the webflasher pipeline against the shared-sdkconfig trap: if the firmware was
built with the wrong board's config (e.g. cardputer's partitions_singleapp_large.csv),
the 'storage' data/fat partition at 0x390000 is missing and the flashed device shows
"mounting sd card failed" + no /ext. Run this on the built partition-table.bin AND on
the final merged bin (which embeds the table at 0x8000) before deploying.

Usage:
  verify_partition_table.py <file> [--offset 0x8000]
    <file>      partition-table.bin (raw table) or a merged flash image
    --offset    where the partition table starts in <file> (0 for a raw
                partition-table.bin, 0x8000 for a merged image; auto-detected
                from {0, 0x8000} if omitted)
"""
import sys

WANT_NAME = "storage"
WANT_OFFSET = 0x390000
ENTRY = 32
TABLE_REGION = 0x1000  # ESP-IDF partition table occupies one 4 KB region
MAGIC = b"\xaa\x50"
MD5_MARKER = b"\xeb\xeb"


def _safe(name_bytes):
    name = name_bytes.split(b"\x00")[0]
    return "".join(chr(b) if 32 <= b < 127 else "?" for b in name)


def parse(data, base):
    """Parse the single partition table starting at `base`, bounded to one 4 KB
    region and stopping at the MD5 marker or first non-entry (padding). This avoids
    false-positive 0xAA50 matches deeper in a large merged image."""
    parts = []
    end = min(len(data), base + TABLE_REGION)
    i = base
    while i + ENTRY <= end:
        rec = data[i:i + ENTRY]
        if rec[:2] == MAGIC:
            parts.append((_safe(rec[12:28]),
                          int.from_bytes(rec[4:8], "little"),
                          int.from_bytes(rec[8:12], "little")))
        elif rec[:2] == MD5_MARKER:
            break
        else:
            break  # 0xFF padding / end of table
        i += ENTRY
    return parts


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    path = sys.argv[1]
    offset = None
    if "--offset" in sys.argv:
        offset = int(sys.argv[sys.argv.index("--offset") + 1], 0)
    data = open(path, "rb").read()

    candidates = [offset] if offset is not None else [0, 0x8000]
    parts = []
    for base in candidates:
        if base is None or base >= len(data):
            continue
        p = parse(data, base)
        if p:
            parts = p
            break

    print("Partitions found in %s:" % path)
    for name, off, size in parts:
        print("  %-10s off=0x%06X size=0x%06X" % (name, off, size))

    ok = any(n == WANT_NAME and o == WANT_OFFSET for n, o, _ in parts)
    if not ok:
        sys.stderr.write(
            "\nFAIL: no '%s' partition at 0x%06X -- firmware was built with the WRONG "
            "board config (missing PSRAM/storage). Regenerate sdkconfig from the "
            "m5stick defaults and rebuild before deploying.\n" % (WANT_NAME, WANT_OFFSET))
        return 1
    print("\nOK: '%s' partition present at 0x%06X." % (WANT_NAME, WANT_OFFSET))
    return 0


if __name__ == "__main__":
    sys.exit(main())
