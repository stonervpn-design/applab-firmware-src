#!/usr/bin/env python3
"""Replica of tpms_protocol_schrader_gg4_pack (from-scratch TPMS frame).
Validates the encode -> CRC -> decode round-trip so a seeded synthetic frame is
well-formed (a car ECU rejects a wrong CRC). Keep in step with schrader_gg4.c."""
import sys

def crc8(data, poly=0x07, init=0x00):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def pack(sensor_id, pressure_bar, temp_c, status=0x00):
    p_raw = max(0, min(0xFF, round(pressure_bar / (2.5 * 0.069))))
    t_raw = max(0, min(0xFF, round(temp_c + 50.0)))
    b = [
        status & 0xFF,
        (sensor_id >> 24) & 0xFF, (sensor_id >> 16) & 0xFF,
        (sensor_id >> 8) & 0xFF, sensor_id & 0xFF,
        p_raw, t_raw, 0,
    ]
    b[7] = crc8(b[1:7])  # CRC over bytes 1..6
    data = 0
    for x in b:
        data = (data << 8) | x
    return b, data

def unpack(b):
    sid = (b[1] << 24) | (b[2] << 16) | (b[3] << 8) | b[4]
    pressure = b[5] * (2.5 * 0.069)
    temp = b[6] - 50.0
    crc_ok = (crc8(b[1:7]) == b[7])
    return sid, pressure, temp, crc_ok

def main():
    ok = True
    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("OK  " if cond else "FAIL") + " " + msg)

    # default seed used by tpms_spoof.c
    sid, pbar, tc = 0x0CA1B2C3, 2.20, 20.0
    b, data = pack(sid, pbar, tc)
    print(f"  default frame: bytes={' '.join(f'{x:02X}' for x in b)}  data=0x{data:016X}")
    sid2, p2, t2, crc_ok = unpack(b)
    chk(crc_ok, "CRC-8 valid")
    chk(sid2 == sid, f"id round-trips (0x{sid2:08X})")
    chk(abs(p2 - pbar) < 0.2, f"pressure round-trips (~{p2:.2f} bar)")
    chk(abs(t2 - tc) < 1.0, f"temperature round-trips ({t2:.0f} C)")

    # a few more ids/pressures
    for sid, pbar, tc in [(0x00000001, 1.8, -10.0), (0xFFFFFFFF, 3.5, 60.0), (0x12345678, 2.5, 25.0)]:
        b, _ = pack(sid, pbar, tc)
        s2, p2, t2, c = unpack(b)
        chk(c and s2 == sid, f"id 0x{sid:08X} @ {pbar}bar/{tc}C -> ok")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
