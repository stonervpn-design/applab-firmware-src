#!/usr/bin/env python3
"""Fail (exit 1) if a PUBLIC firmware bin contains any car key material.

Per the private-only guardrail: car keystores/keys must NEVER ship in a public
build unless the user explicitly authorizes it with the password 'lemon420'.
This is the last-line automated gate wired into the public build/deploy scripts.

Manufacturer NAME strings (e.g. "AN-Motors", "Allmatic") are PUBLIC and expected
in every Flipper firmware's preset/name tables, so they are NOT flagged — only
actual key MATERIAL is.
"""
import re
import sys

# Byte markers that only appear when real car key material is baked in.
MARKERS = {
    b"01376C86ADABCC43": "VAG AUT64 keystore (head)",
    b"038AA37B1E561F83": "VAG AUT64 keystore (tail)",
    b"Encrypt_data: RAW": "raw keystore blob marker (VAG)",
    b"C0 6B AC 39 D8 F9 F3 60": "VAG test capture 1 key",
    b"C0 D5 C8 CF 0B 83 C7 33": "VAG test capture 2 key",
}
# KeeLoq keystore key-entry lines: Name:16HEXKEY:type
ENTRY = re.compile(rb"[A-Za-z0-9_ .()\-]{2,32}:[0-9A-Fa-f]{16}:[0-9]")


def main():
    if len(sys.argv) < 2:
        print("usage: verify_public_no_car.py <bin>")
        return 2
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()
    hits = [desc for m, desc in MARKERS.items() if m in data]
    if ENTRY.search(data):
        hits.append("KeeLoq keystore key-entry (Name:16HEX:type)")
    if hits:
        print(f"FAIL: CAR KEY MATERIAL FOUND in public bin: {path}")
        for h in hits:
            print(f"  - {h}")
        print("Public builds must be car-free (guardrail). Aborting deploy.")
        return 1
    print(f"PASS: no car key material in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
