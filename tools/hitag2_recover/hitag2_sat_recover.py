#!/usr/bin/env python3
"""Hitag-2 RKE key recovery — Phase 1: SAT attack (HOST, private research).

Encodes the byte-verified Hitag-2 cipher (init + 48-bit LFSR + f20 filter,
identical to hitag2_cipher.c / hitag2_make_forge.py) as CNF (native XOR clauses
for the LFSR via CryptoMiniSat + canonical LUTs for the f20 filter) and solves
for the 48-bit key from captured RKE frames (known UID + nonce + ks32).

MEASURED HONEST RESULT (this is the Phase-0 gate answering itself):
  * The encoding is CORRECT — fixing the true key is instantly SAT, and the
    solver recovers the key in <0.2 s once ~24 key bits are pinned (see
    --hint / self-test).
  * FULL unknown-48-bit RKE recovery by SAT is NOT feasible: 2 frames hang, heavy
    over-constraining (24-96 frames) hangs, and guess-and-SAT hits a 2^g wall
    (per-solve is ms at g=24, but 2^24 guesses = days). This matches the
    literature: efficient Hitag-2 recovery is the SPECIALIZED ~2^35 guess-and-
    determine / cube attack (Verdult-Garcia-Balasch), a major cryptanalytic
    implementation in optimized C/Rust — NOT naive SAT.

So this tool ships as: (a) a verified cipher + SAT oracle, (b) a recovery that
works with a partial-key hint (e.g. bits known from another source), and (c) the
honest negative that closes the Phase-0 gate. It does NOT claim a full break.

Requires:  pip install --user pycryptosat   (XOR-native SAT; CryptoMiniSat)

Run:  python hitag2_sat_recover.py                 # self-test: recover w/ 24-bit hint
      python hitag2_sat_recover.py --hint N        # self-test with N known bits
"""
import sys
import time

from pycryptosat import Solver  # native XOR clauses -> Gaussian elim on the LFSR

M48 = (1 << 48) - 1
F4A, F4B, F5C = 0x2C79, 0x6671, 0x7907287B
TAPS = [0, 2, 3, 6, 7, 8, 16, 22, 23, 26, 30, 41, 42, 43, 46, 47]

# ── reference cipher (for self-test frame generation + final verification) ──


def _i4(x, a, b, c, d):
    return ((x >> a) & 1) | (((x >> b) & 1) << 1) | (((x >> c) & 1) << 2) | (((x >> d) & 1) << 3)


def _f20(x):
    i5 = ((F4A >> _i4(x, 1, 2, 4, 5)) & 1) + ((F4B >> _i4(x, 7, 11, 13, 14)) & 1) * 2 \
        + ((F4B >> _i4(x, 16, 20, 22, 25)) & 1) * 4 + ((F4B >> _i4(x, 27, 28, 30, 32)) & 1) * 8 \
        + ((F4A >> _i4(x, 33, 42, 43, 45)) & 1) * 16
    return (F5C >> i5) & 1


def _round(s):
    x = s[0]
    fb = 0
    for t in TAPS:
        fb ^= (x >> t) & 1
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


# ─────────────────────────── CNF builder ───────────────────────────


class Cms:
    """CryptoMiniSat circuit builder: XOR gates as native XOR clauses (Gaussian
    elimination handles the linear LFSR), f20 as small canonical CNF LUTs."""

    def __init__(self):
        self.n = 0
        self.s = Solver()
        self.c0 = self.new()
        self.s.add_clause([-self.c0])  # constant 0
        self.c1 = self.new()
        self.s.add_clause([self.c1])   # constant 1

    def new(self):
        self.n += 1
        return self.n

    def const(self, bit):
        return self.c1 if bit else self.c0

    def fix(self, v, bit):
        self.s.add_clause([v if bit else -v])

    def xorl(self, lits, rhs=False):
        # y = XOR(lits) XOR rhs  (native XOR clause: XOR(lits, y) == rhs)
        y = self.new()
        self.s.add_xor_clause(lits + [y], rhs)
        return y

    def lut(self, inputs, func):
        # y <-> func(inputs); inputs[0] = LSB. 2^k canonical clauses (k<=5 -> tiny).
        y = self.new()
        k = len(inputs)
        for row in range(1 << k):
            cl = []
            for j in range(k):
                bit = (row >> j) & 1
                cl.append(inputs[j] if bit == 0 else -inputs[j])  # true when input j differs
            cl.append(y if func(row) else -y)
            self.s.add_clause(cl)
        return y

    def f20(self, x):
        y0 = self.lut([x[1], x[2], x[4], x[5]], lambda i: (F4A >> i) & 1)
        y1 = self.lut([x[7], x[11], x[13], x[14]], lambda i: (F4B >> i) & 1)
        y2 = self.lut([x[16], x[20], x[22], x[25]], lambda i: (F4B >> i) & 1)
        y3 = self.lut([x[27], x[28], x[30], x[32]], lambda i: (F4B >> i) & 1)
        y4 = self.lut([x[33], x[42], x[43], x[45]], lambda i: (F4A >> i) & 1)
        return self.lut([y0, y1, y2, y3, y4], lambda i: (F5C >> i) & 1)


def encode_frame(c, K, uid, nonce, ks32):
    """Tie key vars K (=rev(on-air key)) to this frame's ks32."""
    serial = _rev(uid, 32)
    iv = _rev(nonce, 32)
    # init state: x[0..31] = serial bits (const), x[32..47] = K[0..15]
    x = [c.const((serial >> j) & 1) for j in range(32)] + [K[j] for j in range(16)]
    for i in range(32):
        shifted = [x[j + 1] for j in range(47)] + [c.c0]  # bit 47 unused by f20
        f = c.f20(shifted)
        b = c.xorl([f, K[16 + i]], rhs=bool((iv >> i) & 1))  # b = f XOR iv_i XOR key_i
        x = shifted[:47] + [b]
    # keystream: 32 output bits (round order), constrained to ks32
    for r in range(32):
        fb = c.xorl([x[t] for t in TAPS])            # single native XOR clause
        newx = [x[j + 1] for j in range(47)] + [fb]
        out = c.f20(newx)
        byte_b, i = r // 8, r % 8
        expected = ((ks32 >> ((3 - byte_b) * 8)) >> (i ^ 7)) & 1
        c.fix(out, expected)
        x = newx


def recover(frames, hint_bits=0, hint_revlow=0):
    """frames: list of (uid, nonce, ks32). Returns on-air 48-bit key or None.
    hint_bits: number of low bits of rev(on-air key) that are known (pins the
    search so the SAT core solves in ms — full 48-bit unknown is infeasible)."""
    c = Cms()
    K = [c.new() for _ in range(48)]  # rev(on-air key) bits, LSB..MSB
    for j in range(hint_bits):
        c.fix(K[j], (hint_revlow >> j) & 1)
    for uid, nonce, ks32 in frames:
        encode_frame(c, K, uid, nonce, ks32)
    sat, sol = c.s.solve()
    if not sat:
        return None
    revkey = 0
    for j in range(48):
        if sol[K[j]]:
            revkey |= (1 << j)
    return _rev(revkey, 48)  # un-reverse -> on-air key


def selftest(hint=24):
    import os
    print("== Hitag-2 SAT key recovery - self-test (synthetic) ==")
    key = int.from_bytes(os.urandom(6), "big") & M48
    uid = int.from_bytes(os.urandom(4), "big")
    btn = 1
    frames = []
    for ctr in (0x1000, 0x1001, 0x1002):
        nonce = ((ctr & 0x0FFFFFFF) << 4) | btn
        frames.append((uid, nonce, rke_ks(key, uid, ctr, btn)))
    revkey = _rev(key, 48)
    hint_revlow = revkey & ((1 << hint) - 1)  # low `hint` bits of rev(key) "known"
    print(f"   true key = {key:012X}  UID = {uid:08X}  frames={len(frames)}  hint={hint} bits")
    t0 = time.process_time()
    got = recover(frames, hint_bits=hint, hint_revlow=hint_revlow)
    dt = time.process_time() - t0
    ok = got == key
    print(f"   recovered = {got:012X}" if got is not None else "   recovered = UNSAT")
    print(f"   correct = {ok}   solve time = {dt*1000:.0f} ms")
    if ok:
        rep = all(rke_ks(got, u, (n >> 4) & 0x0FFFFFFF, n & 0xF) == k for u, n, k in frames)
        print(f"   recovered key reproduces all frames: {rep}")
    print("\n   NOTE: this proves the SAT encoding + oracle. Full 48-bit unknown recovery")
    print("   by SAT is NOT feasible (measured) -- it needs the specialized ~2^35 cube")
    print("   attack in optimized C/Rust. See the module docstring / design spec.")
    return 0 if ok else 1


def main():
    hint = 24
    for a in sys.argv[1:]:
        if a.startswith("--hint"):
            hint = int(a.split("=", 1)[1]) if "=" in a else int(sys.argv[sys.argv.index(a) + 1])
    hint = max(0, min(48, hint))
    return selftest(hint)


if __name__ == "__main__":
    sys.exit(main())
