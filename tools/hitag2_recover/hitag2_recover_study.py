#!/usr/bin/env python3
"""Hitag-2 RKE key-recovery — Phase 0 breakability study (HONEST feasibility gate).

Uses the byte-verified Hitag-2 cipher (identical to hitag2_make_forge.py /
lib/subghz/protocols/hitag2_cipher.c) to, on SYNTHETIC data with a known key:
  1. prove the recovery ORACLE (a candidate key is confirmed iff it reproduces
     every captured ks32 for the known UID + nonces),
  2. show how many frames uniquely pin the 48-bit key (information-theoretic),
  3. TIME a reduced-unknown-bits brute and extrapolate the full 2^48 naive cost,
  4. print an honest verdict on whether a real attack is Python-feasible or needs
     a dedicated (SAT / guess-and-determine, Rust/GPU) Phase-1 build.

No real captures, no claimed break. This decides whether Phase 1 is worth writing.
Run:  python hitag2_recover_study.py [unknown_bits=24]
"""
import sys
import time

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


def gen_frames(key, uid, n, btn=1, start=0x1000):
    """n synthetic RKE frames: (counter, ks32) for a known key + UID."""
    return [(c, rke_ks(key, uid, c, btn)) for c in range(start, start + n)]


def key_matches(key, uid, frames, btn=1):
    return all(rke_ks(key, uid, c, btn) == ks for c, ks in frames)


def main():
    unknown = int(sys.argv[1]) if len(sys.argv) > 1 else 24
    unknown = max(8, min(28, unknown))  # keep the timing run bounded

    KEY = 0x4F5A6B7C8D9E
    UID = 0x11223344
    BTN = 1

    print("== Hitag-2 RKE key-recovery - Phase 0 breakability study ==")
    print(f"synthetic key = {KEY:012X}  UID = {UID:08X}\n")

    # 1) Oracle: the true key reproduces every frame; a random wrong key does not.
    frames = gen_frames(KEY, UID, 4, BTN)
    assert key_matches(KEY, UID, frames, BTN), "cipher/self-test broken"
    wrong = KEY ^ 1
    print("1) ORACLE  true key reproduces 4 frames:", key_matches(KEY, UID, frames, BTN))
    print("           key^1 (1-bit off) reproduces them:", key_matches(wrong, UID, frames, BTN),
          "(must be False)")

    # 2) Uniqueness: each frame gives 32 keystream bits; a wrong key matches one
    #    frame only with prob 2^-32, so 2 frames pin the 48-bit key info-theoretically.
    print("\n2) UNIQUENESS  each frame constrains 32 bits; 48-bit key needs >=2 frames.")
    print("   P(random wrong key matches k frames) = 2^(-32*k):  1 frame 2^-32, 2 frames 2^-64.")
    print("   -> 2 clean frames uniquely determine the key (information-theoretically).")

    fr = gen_frames(KEY, UID, 2, BTN)

    # 3a) Small-unknown recovery proves the oracle actually recovers the key.
    small = 16
    base = KEY & ~((1 << small) - 1)
    found = None
    for guess in range(1 << small):
        cand = base | guess
        if rke_ks(cand, UID, fr[0][0], BTN) == fr[0][1] and key_matches(cand, UID, fr, BTN):
            found = cand
            break
    ok = found == KEY
    print(f"\n3a) RECOVERY (low {small} bits unknown)  recovered="
          f"{('%012X' % found) if found is not None else None}  correct={ok}")

    # 3b) Clean rate over a fixed sample -> extrapolate the naive 2^48 cost.
    SAMPLE = 100_000
    t0 = time.process_time()
    for guess in range(SAMPLE):
        rke_ks(base | guess, UID, fr[0][0], BTN)  # one keystream per candidate
    dt = time.process_time() - t0
    rate = SAMPLE / dt if dt > 0 else float("inf")
    print(f"3b) RATE  {SAMPLE:,} candidate keystreams in {dt:.2f}s -> ~{rate:,.0f} keys/s "
          "(single Python core)")

    # Extrapolate full 2^48 naive brute at this rate.
    full = (1 << 48) / rate if rate > 0 else float("inf")
    print("\n== VERDICT ==")
    print(f"   naive full 48-bit brute at ~{rate:,.0f} keys/s ~= {full:,.0f} s "
          f"~= {full/86400/365:,.1f} years (single Python core).")
    print("   => Naive brute is INFEASIBLE. 2 frames DO uniquely determine the key, so the")
    print("      information is there; a real recovery needs the guess-and-determine / SAT")
    print("      attack on the LFSR+f20 structure (Phase 1, Rust/pysat), NOT a Python brute.")
    print("   HONEST: Phase 0 confirms the oracle + uniqueness + that Phase 1 must be the")
    print("           efficient algebraic attack. Do NOT ship a 'break' as a brute.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
