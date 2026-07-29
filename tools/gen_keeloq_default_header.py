#!/usr/bin/env python3
"""Rewrite subghz_keeloq_default_keys.h so the compiled-in keeloq default keys
are PUBLIC_BUILD-aware: public gets vehicle systems stripped, private/hardened
keep the full set. Operates in place on the header's OWN entry lines (preserving
manual additions like AN-Motors / Allmatic and exact ordering).

  static const char keeloq_mfcodes_user_default[] =
  "Filetype...\n" "Version: 0\n" "Encryption: 0\n"
  #ifdef PUBLIC_BUILD
    <non-vehicle entries only>
  #else
    <all entries>
  #endif
  ;
Adjacent string literals concatenate, so exactly one block compiles in.
"""
import re
import sys

H = sys.argv[1]

VEHICLE = [
    "kia", "hyundai", "hyu", "subaru", "vag", "toyota", "suzuki", "nissan",
    "honda", "mitsubishi", "chrysler", "daewoo", "audi", "jaguar",
    "starline", "star", "sl_", "pandora", "tomahawk", "tomohawk", "pantera",
    "sheriff", "sherif", "scher", "kgb", "magicar", "cenmax", "mongoose",
    "alligator", "steelmate", "leopard", "partisan", "guard_rf",
    "harpoon", "kh6",
]
KEEP_OVERRIDE = ["ecostar"]  # Hormann garage remote, collides with "star"


def is_vehicle(name):
    low = name.lower()
    if any(k in low for k in KEEP_OVERRIDE):
        return False
    return any(v in low for v in VEHICLE)


lines = open(H, encoding="utf-8").read().split("\n")

# prefix = everything through the 'Encryption: 0' literal; suffix starts at ';'
enc_idx = next(i for i, l in enumerate(lines) if 'Encryption: 0' in l)
entry_re = re.compile(r'^\s*"([0-9A-Fa-f]{16}):[^:]*:(.*)\\n"\s*,?\s*$')

prefix = lines[: enc_idx + 1]
entries, semi_idx = [], None
for i in range(enc_idx + 1, len(lines)):
    l = lines[i]
    if l.strip() == ";":
        semi_idx = i
        break
    if l.strip().startswith('"'):
        entries.append(l)

m_all = [entry_re.match(l) for l in entries]
assert all(m_all), "unparsed entry line(s): " + str(
    [entries[i] for i, m in enumerate(m_all) if not m]
)
keep = [l for l, m in zip(entries, m_all) if not is_vehicle(m.group(2))]
removed = [m.group(2) for l, m in zip(entries, m_all) if is_vehicle(m.group(2))]

# ── Fail-closed guard ────────────────────────────────────────────────────────
# The PUBLIC set (`keep`) must contain ZERO vehicle systems. This is a SECOND,
# independent line of defense beyond is_vehicle(): a broad car-make + car/moto
# alarm-brand blocklist. If the primary classifier ever misses an entry, this
# aborts generation loudly instead of silently rebuilding a car key into the
# public firmware. Deliberately excludes gate/garage brands that legitimately
# stay public (Nice/FAAC/BFT/Came/Beninca/JCM/Hormann-EcoStar/AN-Motors/…).
# Private/hardened builds are unaffected — they always compile the full set.
VEHICLE_STRICT = [
    # car makes
    "audi", "bmw", "mercedes", "benz", "volkswagen", "vw_", "volvo", "ford",
    "chevrolet", "chevy", "gmc", "cadillac", "buick", "chrysler", "dodge", "jeep",
    "fiat", "alfa", "lancia", "peugeot", "citroen", "renault", "opel", "vauxhall",
    "seat", "skoda", "kia", "hyundai", "hyu", "genesis", "toyota", "lexus",
    "honda", "acura", "nissan", "infiniti", "mazda", "subaru", "suzuki",
    "mitsubishi", "daihatsu", "isuzu", "daewoo", "ssangyong", "vag", "jaguar",
    "porsche", "bentley", "tesla", "chery", "geely", "haval", "lada", "gaz", "uaz",
    # car / moto alarm brands
    "starline", "star_line", "sl_", "pandora", "tomahawk", "tomohawk", "pantera",
    "sheriff", "sherif", "scher", "kgb", "magicar", "cenmax", "mongoose",
    "alligator", "steelmate", "leopard", "partisan", "guard_rf", "harpoon", "kh6",
    "pharaon", "prizrak", "convoy", "blackbug", "black_bug", "red_scorpio",
    "fantom", "phantom", "cyclon", "mangust", "mongust", "excellent", "mystery",
]


def looks_vehicle_strict(name):
    low = name.lower()
    if any(k in low for k in KEEP_OVERRIDE):
        return False
    return any(v in low for v in VEHICLE_STRICT)


leaked = sorted({m.group(2) for m in m_all if not is_vehicle(m.group(2)) and looks_vehicle_strict(m.group(2))})
if leaked:
    sys.exit(
        "ABORT: the PUBLIC keystore would still contain vehicle systems the primary "
        "filter missed:\n  " + "\n  ".join(leaked) +
        "\nAdd them to VEHICLE (or KEEP_OVERRIDE if a false match) and re-run."
    )

out = []
out += prefix
out.append("#ifdef PUBLIC_BUILD")
out += keep
out.append("#else")
out += entries
out.append("#endif")
out.append(";")
out += lines[semi_idx + 1 :]

open(H, "w", encoding="utf-8", newline="\n").write("\n".join(out))
sys.stderr.write(
    "rewrote %s: %d total entries, PUBLIC keeps %d, removes %d vehicle\n"
    % (H, len(entries), len(keep), len(removed))
)
sys.stderr.write("removed: " + ", ".join(sorted(set(removed))) + "\n")
