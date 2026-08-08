#!/usr/bin/env python3
"""Replica of tpms_protocol_ford_pack / _analyze — validates the from-scratch
Ford TPMS frame round-trips (fields -> bytes + sum checksum -> fields). Keep in
step with tpms_ford.c."""
import sys

def pack(sensor_id, pressure_bar, temp_c):
    psi_q = max(0, min(0x1FF, round(pressure_bar / 0.06895 / 0.25)))
    t = max(0, min(0x7F, round(temp_c) + 56))
    b = [
        (sensor_id >> 24) & 0xFF, (sensor_id >> 16) & 0xFF,
        (sensor_id >> 8) & 0xFF, sensor_id & 0xFF,
        psi_q & 0xFF,
        t & 0x7F,
        0x20 if (psi_q & 0x100) else 0x00,
        0,
    ]
    b[7] = sum(b[0:7]) & 0xFF
    return b

def analyze(b):
    sid = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
    psi_bits = ((b[6] & 0x20) << 3) | b[4]      # bit5 of b[6] -> bit8
    bar = psi_bits * 0.25 * 0.06895
    temp = (b[5] & 0x7F) - 56 if (b[5] & 0x80) == 0 else None
    csum_ok = (sum(b[0:7]) & 0xFF) == b[7]
    return sid, bar, temp, csum_ok

def main():
    ok = True
    def chk(c, m):
        nonlocal ok; ok = ok and c; print(("OK  " if c else "FAIL") + " " + m)

    for sid, bar, tc in [(0x0089ABCD, 2.20, 20), (0x00000001, 1.8, -10),
                         (0x12345678, 3.0, 40), (0xDEADBEEF, 2.5, 0)]:
        b = pack(sid, bar, tc)
        s2, b2, t2, ck = analyze(b)
        chk(ck, f"id 0x{sid:08X} checksum valid  (frame {' '.join(f'{x:02X}' for x in b)})")
        chk(s2 == sid, f"  id round-trips 0x{s2:08X}")
        chk(abs(b2 - bar) < 0.2, f"  pressure ~{b2:.2f} bar")
        chk(t2 is not None and abs(t2 - tc) <= 1, f"  temp {t2} C")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
