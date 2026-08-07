#!/usr/bin/env python3
"""Validation replica of subghz_chip_id.c (the pure encoder-chip classifier).

No local gcc on this machine -> this Python mirror is the executable check for
the C mapping logic. Keep it byte-for-byte in step with subghz_chip_id.c.
Precondition (as in C): only called for FIXED-code decoded protocol names.
"""
import sys


def has(haystack, needle):
    return haystack is not None and needle in haystack


def chip_identify(proto_name, bits):
    r = {"label": "", "family": "Unknown", "total_bits": bits, "fixed": False}
    if has(proto_name, "Princeton"):
        r.update(family="Princeton", label="Princeton (PT2262/EV1527)", fixed=True)
    elif has(proto_name, "CAME"):
        r.update(family="Came", label="CAME", fixed=True)
    elif has(proto_name, "Nice"):
        r.update(family="Nice", label="NICE FLO", fixed=True)
    elif has(proto_name, "Holtek"):
        r.update(family="Holtek", label="Holtek HT12/HT6", fixed=True)
    elif has(proto_name, "Ansonic"):
        r.update(family="Ansonic", label="Ansonic", fixed=True)
    elif has(proto_name, "Chamberlain"):
        r.update(family="Chamberlain", label="Chamberlain", fixed=True)
    elif has(proto_name, "Linear") and has(proto_name, "Delta"):
        r.update(family="LinearDelta", label="Linear Delta", fixed=True)
    elif has(proto_name, "Linear"):
        r.update(family="Linear", label="Linear", fixed=True)
    return r


CASES = [
    # (name, bits, expected_family, expected_fixed)
    ("Princeton", 24, "Princeton", True),
    ("CAME", 12, "Came", True),
    ("Nice FLO", 12, "Nice", True),
    ("Holtek HT12", 12, "Holtek", True),
    ("Ansonic", 12, "Ansonic", True),
    ("Chamberlain", 8, "Chamberlain", True),
    ("Linear Delta", 8, "LinearDelta", True),
    ("Linear", 10, "Linear", True),
    ("KeeLoq", 64, "Unknown", False),
    (None, 0, "Unknown", False),
]


def main():
    ok = True
    for name, bits, fam, fixed in CASES:
        r = chip_identify(name, bits)
        good = r["family"] == fam and r["fixed"] == fixed and r["total_bits"] == bits
        ok = ok and good
        print(("OK  " if good else "FAIL") +
              " name=%r -> family=%s fixed=%s label=%r" % (name, r["family"], r["fixed"], r["label"]))
    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
