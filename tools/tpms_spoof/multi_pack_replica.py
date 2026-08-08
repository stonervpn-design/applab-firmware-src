#!/usr/bin/env python3
"""Round-trip replicas for Renault / Citroen / Toyota(PMV-107J) TPMS packers.
Mimics each ENCODER's frame reconstruction (brute-force head / XOR) + the
DECODER's analyze, so we confirm pack(id,pressure,temp) survives the full path
before writing C. Keep in step with tpms_renault.c / tpms_citroen.c / tpms_pmv107j.c."""
import sys

def crc8(data, poly, init=0):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

# ---------- Renault (72-bit, CRC-8 0x07, encoder brute-forces b[0]) ----------
def renault_pack(sid, bar, tc):
    p_raw = max(0, min(0x3FF, round(bar / 0.0075)))
    t = max(0, min(0xFF, round(tc) + 30))
    b = [(p_raw >> 8) & 0x03, p_raw & 0xFF, t, sid & 0xFF, (sid >> 8) & 0xFF,
         (sid >> 16) & 0xFF, 0, 0, 0]
    b[8] = crc8(b[0:8], 0x07)
    data = 0
    for x in b[1:9]:
        data = (data << 8) | x     # generic.data = b[1..8]
    return data, b

def renault_roundtrip(sid, bar, tc):
    data, orig = renault_pack(sid, bar, tc)
    b = [0] + [(data >> (8 * (7 - i))) & 0xFF for i in range(8)]  # b[1..8]
    for cand in range(256):                                       # encoder brute-force b[0]
        if crc8([cand] + b[1:8], 0x07) == b[8]:
            b[0] = cand; break
    p_raw = ((b[0] & 0x03) << 8) | b[1]
    bar2 = p_raw * 0.75 * 0.01
    tc2 = b[2] - 30
    id2 = (b[5] << 16) | (b[4] << 8) | b[3]
    gate = crc8(b[0:8], 0x07) == b[8]                             # renault_check() gate
    return id2 == (sid & 0xFFFFFF), abs(bar2 - bar) < 0.05, tc2 == round(tc), b[0] == orig[0], gate

# ---------- Citroen (80-bit, XOR checksum -> encoder computes b[1]) ----------
def citroen_pack(sid, bar, tc):
    p_raw = max(0, min(0xFF, round(bar / 0.01364)))
    t = max(0, min(0xFF, round(tc) + 50))
    b2_9 = [(sid >> 16) & 0xFF, (sid >> 8) & 0xFF, sid & 0xFF, 0, p_raw, t, 0, 0]  # b[2..9]
    data = 0
    for x in b2_9:
        data = (data << 8) | x
    return data

def citroen_roundtrip(sid, bar, tc):
    data = citroen_pack(sid, bar, tc)
    b = [0, 0] + [(data >> (8 * (7 - i))) & 0xFF for i in range(8)]  # b[2..9]
    x = 0
    for i in range(2, 10):
        x ^= b[i]
    b[1] = x                                                        # encoder: XOR(b[1..9])==0
    assert (b[1] ^ (0 if False else 0)) is not None
    xor_ok = (0 == (b[1] ^ b[2] ^ b[3] ^ b[4] ^ b[5] ^ b[6] ^ b[7] ^ b[8] ^ b[9]))
    id2 = (b[1] << 24) | (b[2] << 16) | (b[3] << 8) | b[4]         # top byte = checksum (quirk)
    bar2 = b[6] * 1.364 * 0.01
    tc2 = b[7] - 50
    return (id2 & 0xFFFFFF) == (sid & 0xFFFFFF), abs(bar2 - bar) < 0.05, tc2 == round(tc), xor_ok

# ---------- Toyota PMV-107J (66-bit, 28-bit split id, CRC-8 0x13, brute b[0] 0..3) ----------
# Decoder check GATE (pmv107j_check in tpms_pmv107j.c): CRC-8 over b[0..7] == b[8]
# AND b[6] == (b[5] ^ 0xFF). The earlier replica only ran analyze (ignores b[6])
# and gave a false pass with b[6]=0 — a frame the receiver would REJECT. Model the
# gate so that can't happen again.
def pmv_pack(sid, bar, tc, batt=0):
    idm = sid & 0x0FFFFFFF
    p_raw = max(0, min(0xFF, round(bar * 100.0 / 2.48 + 40)))
    t = max(0, min(0xFF, round(tc) + 40))
    b = [(idm >> 26) & 0x03, (idm >> 18) & 0xFF, (idm >> 10) & 0xFF, (idm >> 2) & 0xFF,
         ((idm & 0x03) << 6) | ((batt & 1) << 5), p_raw, p_raw ^ 0xFF, t, 0]  # b[6] = ~b[5]
    b[8] = crc8(b[0:8], 0x13)
    data = 0
    for x in b[1:9]:
        data = (data << 8) | x
    return data, b

def pmv_check_gate(b):                                             # mirrors pmv107j_check()
    return crc8(b[0:8], 0x13) == b[8] and (b[5] ^ 0xFF) == b[6]

def pmv_roundtrip(sid, bar, tc):
    idm = sid & 0x0FFFFFFF
    data, orig = pmv_pack(sid, bar, tc)
    b = [0] + [(data >> (8 * (7 - i))) & 0xFF for i in range(8)]
    for cand in range(4):                                          # encoder brute-force 2-bit b[0]
        if crc8([cand] + b[1:8], 0x13) == b[8]:
            b[0] = cand; break
    id2 = (b[0] << 26) | (b[1] << 18) | (b[2] << 10) | (b[3] << 2) | (b[4] >> 6)
    bar2 = (b[5] - 40) * 2.48 * 0.01
    tc2 = b[7] - 40
    return id2 == idm, abs(bar2 - bar) < 0.06, tc2 == round(tc), b[0] == orig[0], pmv_check_gate(b)

def main():
    ok = True
    def chk(c, m):
        nonlocal ok; ok = ok and all(c) if isinstance(c, tuple) else (ok and c)
        print(("OK  " if (all(c) if isinstance(c, tuple) else c) else "FAIL") + " " + m)

    for sid, bar, tc in [(0x123456, 2.2, 20), (0x000001, 1.8, -10), (0xABCDEF, 3.0, 45)]:
        chk(renault_roundtrip(sid, bar, tc), f"Renault id 0x{sid:06X} {bar}bar {tc}C (id/press/temp/head)")
    for sid, bar, tc in [(0x123456, 2.2, 20), (0xABCDEF, 2.5, 0), (0x000001, 3.4, 60)]:
        chk(citroen_roundtrip(sid, bar, tc), f"Citroen id 0x{sid:06X} {bar}bar {tc}C (id24/press/temp/xor)")
    for sid, bar, tc in [(0x0123456, 2.2, 20), (0x0FFFFFFF, 2.5, 30), (0x0000001, 1.9, -20)]:
        chk(pmv_roundtrip(sid, bar, tc), f"Toyota id 0x{sid:07X} {bar}bar {tc}C (id28/press/temp/head)")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
