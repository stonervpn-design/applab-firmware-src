#!/usr/bin/env python3
"""Classify keeloq_mfcodes_user entries as VEHICLE (car makes + car-alarm
systems) vs KEEP (gate/garage/barrier + generic algorithm types).

Prints the removal set grouped, the ambiguous set, and the resulting counts.
Does NOT modify anything — inspection only. `--write <out>` emits the filtered
keystore (header + non-vehicle entries)."""
import re
import sys

SRC = sys.argv[1]
WRITE = sys.argv[2] if len(sys.argv) > 2 else None

# Substrings (lowercased) that mark a VEHICLE (car make OR car/moto alarm brand).
VEHICLE = [
    # car makes
    "kia", "hyundai", "hyu", "subaru", "vag", "toyota", "suzuki", "nissan",
    "honda", "mitsubishi", "chrysler", "daewoo", "audi", "jaguar",
    # car / motorcycle alarm systems
    "starline", "star", "sl_", "pandora", "tomahawk", "tomohawk", "pantera",
    "sheriff", "sherif", "scher", "kgb", "magicar", "cenmax", "mongoose",
    "alligator", "steelmate", "leopard", "partisan", "guard_rf",
    # user-confirmed car alarms (2026-07-27):
    "harpoon", "kh6",
]
# Genuinely uncertain — kept per user decision (gate operator).
AMBIGUOUS = ["stilmatic"]
# Force-KEEP: substring collides with a VEHICLE token but is NOT a vehicle.
# EcoStar = Hormann garage-door remote (matched "star").
KEEP_OVERRIDE = ["ecostar"]


def classify(name):
    low = name.lower()
    for k in KEEP_OVERRIDE:
        if k in low:
            return "keep"
    for a in AMBIGUOUS:
        if a in low:
            return "ambiguous"
    for v in VEHICLE:
        if v in low:
            return "vehicle"
    return "keep"


header, entries = [], []
with open(SRC, encoding="utf-8", errors="replace") as f:
    for line in f:
        s = line.rstrip("\n")
        if re.match(r"^[0-9A-Fa-f]{16}:", s):
            entries.append(s)
        else:
            header.append(s)

buckets = {"vehicle": [], "keep": [], "ambiguous": []}
for e in entries:
    name = e.split(":", 2)[2] if e.count(":") >= 2 else e
    buckets[classify(name)].append((name, e))

def names(bucket):
    seen = []
    for n, _ in buckets[bucket]:
        if n not in seen:
            seen.append(n)
    return seen

print(f"total entries: {len(entries)}")
print(f"  VEHICLE (remove): {len(buckets['vehicle'])} entries, {len(names('vehicle'))} systems")
print(f"  AMBIGUOUS:        {len(buckets['ambiguous'])} entries, {len(names('ambiguous'))} systems")
print(f"  KEEP:             {len(buckets['keep'])} entries, {len(names('keep'))} systems")
print("\n--- VEHICLE systems to REMOVE ---")
print(", ".join(sorted(names("vehicle"))))
print("\n--- AMBIGUOUS (your call) ---")
print(", ".join(sorted(names("ambiguous"))) or "(none)")

if WRITE:
    # Keep header + everything not classified vehicle (ambiguous stays for now).
    keep_lines = header + [e for _, e in buckets["keep"]] + [e for _, e in buckets["ambiguous"]]
    with open(WRITE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(keep_lines) + "\n")
    print(f"\nwrote {WRITE}: {len(keep_lines)} lines "
          f"({len(buckets['keep']) + len(buckets['ambiguous'])} entries kept)")
