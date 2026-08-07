#!/usr/bin/env python3
"""Validation replica of subghz_spectrum_util.c (panadapter math).
No local gcc -> this Python mirror is the executable check. Keep in step with the C."""
import sys

RSSI_FLOOR = -97.0
RSSI_CEIL = -60.0


def bin_freq(band_start, bin_step, i):
    return band_start + bin_step * i


def rssi_q(dbm, height):
    if dbm <= RSSI_FLOOR:
        return 0
    if dbm >= RSSI_CEIL:
        return height
    f = (dbm - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR)
    h = int(f * height + 0.5)
    return max(0, min(height, h))


def main():
    ok = True

    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("OK  " if cond else "FAIL") + " " + msg)

    chk(bin_freq(433000000, 13600, 0) == 433000000, "bin0")
    chk(bin_freq(433000000, 13600, 10) == 433136000, "bin10")
    chk(bin_freq(315000000, 20000, 5) == 315100000, "bin5@315")
    chk(rssi_q(-97.0, 26) == 0, "floor")
    chk(rssi_q(-120.0, 26) == 0, "below floor")
    chk(rssi_q(-60.0, 26) == 26, "ceil")
    chk(rssi_q(-40.0, 26) == 26, "above ceil")
    chk(12 <= rssi_q(-78.5, 26) <= 14, "midpoint ~13")
    chk(rssi_q(-90.0, 26) < rssi_q(-70.0, 26), "monotonic")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
