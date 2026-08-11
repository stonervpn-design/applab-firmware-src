import sys, time
sys.path.insert(0, "tools/hitag2_recover")
import hitag2_sat_recover as H
from pycryptosat import Solver

M48 = (1 << 48) - 1
TAPS = H.TAPS


def invert_init(x32, uid, nonce):
    """Deterministically invert the 32-round init: x32 + known serial/nonce -> on-air key."""
    serial = H._rev(uid, 32)
    iv = H._rev(nonce, 32)
    x = x32
    key_hi = 0  # key[16:48], key[16+i] at bit i
    for i in range(31, -1, -1):
        b = (x >> 47) & 1
        shifted = x & ~(1 << 47) & M48
        f = H._f20(shifted)
        kbit = b ^ f ^ ((iv >> i) & 1)
        key_hi |= (kbit << i)
        x = ((shifted << 1) | ((serial >> i) & 1)) & M48
    if (x & 0xFFFFFFFF) != serial:
        return None  # inconsistent x32
    key_lo = (x >> 32) & 0xFFFF
    revkey = key_lo | (key_hi << 16)
    return H._rev(revkey, 48)  # on-air key


def x32_of(key, uid, nonce):
    return H._init(H._rev(key, 48), H._rev(uid, 32), H._rev(nonce, 32))


# ---- 1) validate the init inversion ----
key = 0x4F5A6B7C8D9E
uid = 0x11223344
btn = 1
ctr = 0x1000
nonce = ((ctr & 0x0FFFFFFF) << 4) | btn
x32 = x32_of(key, uid, nonce)
got = invert_init(x32, uid, nonce)
print(f"1) init inversion: x32 -> key = {got:012X}  correct = {got == key}", flush=True)


# ---- 2) keystream-only x32 recovery: encode ONLY the 32 keystream rounds from a
#         free x32 (no init) constrained to ks32, and time a single solve. ----
def encode_ks_only(c, X, ks32):
    x = list(X)
    for r in range(32):
        fb = c.xorl([x[t] for t in TAPS])
        newx = [x[j + 1] for j in range(47)] + [fb]
        out = c.f20(newx)
        byte_b, i = r // 8, r % 8
        expected = ((ks32 >> ((3 - byte_b) * 8)) >> (i ^ 7)) & 1
        c.fix(out, expected)
        x = newx


ks32 = H.rke_ks(key, uid, ctr, btn)
c = H.Cms()
X = [c.new() for _ in range(48)]
encode_ks_only(c, X, ks32)
t = time.process_time()
sat, sol = c.s.solve()
dt = time.process_time() - t
if sat:
    xr = sum((1 << j) for j in range(48) if sol[X[j]])
    krec = invert_init(xr, uid, nonce)
    print(f"2) ks-only solve: SAT in {dt*1000:.0f} ms; x32={xr:012X}; inverts to "
          f"{krec:012X if krec is not None else 0} (== true key: {krec == key})", flush=True)
else:
    print(f"2) ks-only solve: UNSAT in {dt*1000:.0f} ms (encoding bug)", flush=True)
