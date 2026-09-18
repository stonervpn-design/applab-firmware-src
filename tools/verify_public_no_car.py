#!/usr/bin/env python3
"""Fail (exit 1) if a PUBLIC firmware bin contains any car material.

Per the private-only guardrail: NO car material may ship in a public build
unless the owner explicitly authorizes it with the password 'Romandoe420'. This is
the last-line automated gate wired into the public build/deploy scripts.

It scans for THREE classes of leak (the 2026-08-06 audit showed the old version
only checked the first and missed the real leak):

  1. Car KEY material  — VAG AUT64 keystore bytes, KeeLoq keystore key-entry
     lines (format is HEX16:type:Name, e.g. 0000000023304758:2:KGB/Subaru).
  2. Car DECODER code   — ProtoPirate app + per-decoder/protocol display strings
     that only exist when live car decode/encode code is linked in.
  3. Car MODEL names     — vehicle make/model strings from the signal-fingerprint
     table (metadata, but still "car preset names" the owner forbids on public).

Manufacturer NAME strings that legitimately appear in every Flipper's generic
gate/garage tables (e.g. "AN-Motors", "Allmatic", "Nice", "CAME") are PUBLIC and
NOT flagged. Only car-specific vehicle material is.
"""
import os
import re
import sys

# Owner override: when this password is supplied via the PUBLIC_CAR_AUTH env var,
# the gate reports what it found but does NOT fail — used for the deliberately
# car-authorized public applab builds (proto_pirate). Empty/other value = enforce.
AUTH_PASSWORD = "Romandoe420"

# --- class 1: car KEY material (exact byte markers) --------------------------
KEY_MARKERS = {
    b"01376C86ADABCC43": "VAG AUT64 keystore (head)",
    b"038AA37B1E561F83": "VAG AUT64 keystore (tail)",
    b"Encrypt_data: RAW": "raw keystore blob marker (VAG)",
    b"C0 6B AC 39 D8 F9 F3 60": "VAG test capture 1 key",
    b"C0 D5 C8 CF 0B 83 C7 33": "VAG test capture 2 key",
}
# KeeLoq keystore key-entry line: HEX16:type:Name  (was reversed before -> never fired).
# The type is a small enum (0..~15), so 1-2 digits; a 3+ digit middle field is FAT
# wear-level metadata (hash:size:path), not a key line. Name must start with a letter.
KEELOQ_ENTRY = re.compile(rb"(?<![0-9A-Fa-f])[0-9A-Fa-f]{16}:[0-9]{1,2}:[A-Za-z][A-Za-z0-9_/ .()\-]{1,31}")

# --- class 2/3: car DECODER + MODEL names (RKE-specific) ----------------------
# EXACT Sub-GHz car-RKE decoder display names (from the 66 excluded protocol headers)
# plus the car feature/app names. These are version-suffixed / feature-specific on
# purpose: bare brand names ("Chevrolet", "Nissan", ...) legitimately appear in the
# NFC key-copier's transponder table and in wolfSSL ("PSA" = Platform Security Arch),
# so we match "ChevroletV0"/"Nissan V0"/"PSA Brute", never the bare brand.
CAR_NAME_MARKERS = [
    # feature / app
    b"ProtoPirate", b"proto_pirate", b"VAG Fob", b"PSA Brute", b"psa_bf_", b"AUT64",
    # exact decoder names
    b"Audi V1", b"Audi V2", b"Audi V3", b"BMW CAS4", b"BMW_v2", b"BMW_v3",
    b"ChevroletV0", b"Chrysler V0", b"Ducato V0", b"FIAT SPA", b"Fiat Multi",
    b"Fiat V1", b"Ford V0", b"Ford V1", b"Ford V2", b"Ford V3", b"Hitag2",
    b"Honda Static", b"HondaV10", b"HondaV2&V3", b"HondaV4", b"HondaV5", b"HondaV6",
    b"HondaV7", b"HondaV8", b"HondaV9", b"Hyundai TL", b"Hyundai/Kia RIO", b"Jeep V0",
    b"Kia V0", b"Kia V1", b"Kia V2", b"Kia V3/V4", b"Kia V5", b"Kia V6", b"Kia V7",
    b"LDV V0", b"Land Rover V0", b"Lexus V0", b"Mazda V0", b"Mazda V1", b"Mazda V2",
    b"MazdaSiemens", b"Mitsubishi V0", b"Mitsubishi V1", b"Nissan V0", b"Opel V0",
    b"Peugot V0", b"Porsche Touareg", b"Renault V1", b"Renault V2", b"Renault V3",
    b"Renault V4", b"Scher-Khan", b"Sheriff CFM", b"Star Line", b"Subaru", b"Toyota V0",
]


def scan(data, check_keeloq=True):
    """Return the list of car-material hits in a blob."""
    hits = [d for m, d in KEY_MARKERS.items() if m in data]
    if check_keeloq and KEELOQ_ENTRY.search(data):
        hits.append("KeeLoq keystore key-entry line (HEX:type:Name)")
    hits += [f"car RKE name {m!r}" for m in CAR_NAME_MARKERS if m in data]
    return hits


def _iter_paths(path):
    """Yield each file to scan: a directory is walked recursively, a file yields itself."""
    if os.path.isdir(path):
        for dirpath, _, filenames in os.walk(path):
            for fn in sorted(filenames):
                yield os.path.join(dirpath, fn)
    else:
        yield path


def main():
    # Flags may precede/among the paths. --skip-keeloq drops the KeeLoq key-entry
    # check: use it ONLY for the SD-content scan, where keeloq_mfcodes_user (gate /
    # garage manufacturer codes, dual-use, legitimately public) would otherwise
    # false-positive. The firmware-binary scan keeps the full check.
    check_keeloq = "--skip-keeloq" not in sys.argv[1:]
    paths = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not paths:
        print("usage: verify_public_no_car.py [--skip-keeloq] <bin|dir> [<bin|dir> ...]")
        return 2
    authorized = os.environ.get("PUBLIC_CAR_AUTH", "") == AUTH_PASSWORD
    if authorized:
        print("NOTE: PUBLIC_CAR_AUTH matches — owner-authorized car material on "
              "public. Findings are reported below but do NOT abort the deploy.")
    overall = 0
    scanned = 0
    for arg in paths:
        for path in _iter_paths(arg):
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                print(f"ERROR: cannot read {path}: {e}")
                overall = 1
                continue
            scanned += 1
            hits = scan(data, check_keeloq=check_keeloq)
            if hits:
                label = "CAR MATERIAL FOUND (authorized)" if authorized else "FAIL: CAR MATERIAL FOUND"
                print(f"{label} in {path}:")
                for h in sorted(set(hits)):
                    print(f"  - {h}")
                if not authorized:
                    overall = 1
    if scanned and not overall:
        print(f"PASS: no car material ({scanned} file(s) scanned).")
    if overall:
        print("Public builds must be car-free (guardrail). Aborting deploy.")
    return overall


if __name__ == "__main__":
    sys.exit(main())
