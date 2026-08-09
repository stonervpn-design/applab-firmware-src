#!/usr/bin/env python3
"""Replica of subghz_scene_rolling_analyzer_can_clone — validates the Phase 2
secure-learning gate: BFT/Erreka are cloneable ONLY when a programming frame
carried a real (non-zero) seed ("Sd:"); a normal press (Sd:00000000) is not; and
the normal-make counter gate is unchanged. Keep in step with
applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c."""
import re
import sys


def hex_after(text, marker):
    # Mirror subghz_rollback_hex_after: find the marker, parse the hex after it.
    i = text.find(marker)
    if i < 0:
        return 0
    rest = text[i + len(marker):].lstrip()
    m = re.match(r"[0-9A-Fa-f]+", rest)
    return int(m.group(0), 16) if m else 0


def can_clone(protocol, text):
    if protocol != "KeeLoq":
        return False
    if not text:
        return False
    if "MF:Unknown" in text:
        return False
    if "MF:BFT" in text or "MF:Erreka" in text:
        return hex_after(text, "Sd:") != 0
    return "Cnt:" in text and "Cnt:????" not in text


def main():
    ok = True

    def chk(c, m):
        nonlocal ok
        ok = ok and c
        print(("OK  " if c else "FAIL") + " " + m)

    cases = [
        # (protocol, decoded_text, expected)
        ("KeeLoq", "Fix:12345678 MF:Nice Cnt:0A2B", True),        # normal make + counter
        ("KeeLoq", "Fix:12345678 MF:Unknown Cnt:0A2B", False),    # unknown mfr
        ("KeeLoq", "Fix:12345678 MF:Nice Cnt:????", False),       # counter not recovered
        ("KeeLoq", "Fix:12345678 MF:BFT Sd:00000000", False),     # BFT normal press (no seed)
        ("KeeLoq", "Fix:12345678 MF:BFT Sd:1A2B3C4D", True),      # BFT programming frame (real seed)
        ("KeeLoq", "Fix:12345678 MF:Erreka Sd:DEADBEEF", True),   # Erreka with seed
        ("KeeLoq", "Fix:12345678 MF:Erreka Sd:00000000", False),  # Erreka no seed
        ("KeeLoq", "Fix:12345678 MF:AN-Motors PsCn:0A2B", False), # AN-Motors: no Cnt: marker
        ("Princeton", "anything", False),                         # not KeeLoq
    ]
    for proto, text, expect in cases:
        got = can_clone(proto, text)
        chk(got == expect, f"{proto:9} '{text}' -> {got} (want {expect})")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
