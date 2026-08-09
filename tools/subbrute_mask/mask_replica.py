#!/usr/bin/env python3
"""Replica of subbrute_mask.c — validates the function-mask candidate generator:
bijection over the masked bits, fixed bits NEVER altered, max_value == 2^k.
Keep in step with applications/main/subghz/helpers/subbrute_mask.c."""
import sys

M64 = (1 << 64) - 1


def var_bits(mask):
    return bin(mask & M64).count("1")


def max_value(mask):
    k = var_bits(mask)
    if k == 0 or k > 63:
        return 0
    return 1 << k


def candidate(fixed, mask, step):
    result = fixed & ~mask & M64
    m = mask & M64
    while m:
        low = m & (-m)  # lowest set bit
        if step & 1:
            result |= low
        step >>= 1
        m &= m - 1
    return result & M64


def main():
    ok = True

    def chk(c, m):
        nonlocal ok
        ok = ok and c
        print(("OK  " if c else "FAIL") + " " + m)

    tests = [
        (0x00000000, 0x0000000F),  # 4 low bits variable
        (0xABCD0000, 0x0000FFFF),  # low 16 variable, high fixed
        (0x0F0F0F0F, 0xF0F0F0F0),  # interleaved fixed/variable (16 var bits)
        (0x00000000, 0x8000000000000001),  # split ends: bit0 + bit63
        (0x00089ABC, 0x00000700),  # 3-bit button/function field (typical chip-ID mask)
    ]
    for fixed, mask in tests:
        k = var_bits(mask)
        mv = max_value(mask)
        chk(mv == (1 << k), f"mask {mask:#018x}: max_value == 2^{k} = {mv}")

        seen = set()
        fixed_bits = fixed & ~mask & M64
        bijection = True
        fixed_untouched = True
        for step in range(mv):
            c = candidate(fixed, mask, step)
            key = c & mask
            if key in seen:
                bijection = False
            seen.add(key)
            if (c & ~mask & M64) != fixed_bits:
                fixed_untouched = False
        chk(bijection and len(seen) == mv, f"  bijection over all {mv} masked values")
        chk(fixed_untouched, "  fixed (non-masked) bits never altered")
        chk(candidate(fixed, mask, 0) == fixed_bits, "  step 0 == fixed bits only")
        chk(candidate(fixed, mask, mv - 1) == (fixed_bits | (mask & M64)),
            "  last step == fixed | mask")

    # Degenerate masks must report 0 (caller rejects, never sweeps).
    chk(max_value(0) == 0, "empty mask -> max_value 0 (rejected)")
    chk(max_value(M64) == 0, "64-bit mask -> 0 (would overflow, rejected)")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
