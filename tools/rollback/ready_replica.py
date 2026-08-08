#!/usr/bin/env python3
"""Validation replica of subghz_rollback_ready_n (RollBack readiness gate).
No local gcc -> this Python mirror is the executable check. Keep in step with
subghz_rollback_core.c. A store is a list of (serial, counter) slots."""
import sys

MAX_SLOTS = 8


def ready_n(slots, target_n):
    if target_n < 2:
        target_n = 2
    seen = set()
    for serial, _ in slots:
        if serial in seen:
            continue
        seen.add(serial)
        cnt = sorted(c for s, c in slots if s == serial)
        if len(cnt) < target_n:
            continue
        if target_n <= 2:
            return serial
        # constant-step run of length >= target_n
        a = 0
        while a + target_n <= len(cnt):
            step = cnt[a + 1] - cnt[a]
            if step != 0:
                run = 2
                b = a + 1
                while b + 1 < len(cnt) and cnt[b + 1] - cnt[b] == step:
                    run += 1
                    b += 1
                if run >= target_n:
                    return serial
            a += 1
    return None


def main():
    ok = True

    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("OK  " if cond else "FAIL") + " " + msg)

    S = 0x777
    chk(ready_n([(S, 5), (S, 6)], 2) == S, "pair same-serial -> ready at N=2")
    chk(ready_n([(1, 5), (2, 5)], 2) is None, "different serials -> not ready")
    chk(ready_n([(S, 5), (S, 6)], 3) is None, "only 2 -> not ready at N=3")
    chk(ready_n([(S, 5), (S, 6), (S, 7)], 3) == S, "5,6,7 -> ready at N=3 (Mazda)")
    chk(ready_n([(0x888, 5), (0x888, 6), (0x888, 50)], 3) is None, "5,6,50 gap -> not ready N=3")
    chk(ready_n([(0x888, 5), (0x888, 6), (0x888, 50)], 2) == 0x888, "...but 5,6 pair ready N=2")
    chk(ready_n([(0x999, 5), (0x999, 10), (0x999, 15)], 3) == 0x999, "step-5 5,10,15 -> ready N=3")
    chk(ready_n([(S, 5), (S, 6), (S, 7), (S, 8)], 3) == S, "5,6,7,8 -> ready N=3")
    chk(ready_n([(S, 1), (S, 2), (S, 9), (S, 10), (S, 11)], 3) == S, "run 9,10,11 amid noise -> ready")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
