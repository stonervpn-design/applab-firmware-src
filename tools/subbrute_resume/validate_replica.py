#!/usr/bin/env python3
"""Validation replica of subbrute_resume_is_valid (pure resumable-brute predicate).
No local gcc -> this Python mirror is the executable check. Keep in step with the C
and with the SubBruteAttacks enum (subbrute_protocols.h)."""
import sys

SCHEMA = 1
ATTACK_TOTAL = 45     # SubBruteAttackTotalCount
ATTACK_DEBRUIJN = 43  # SubBruteAttackDeBruijn
ATTACK_LOADFILE = 44  # SubBruteAttackLoadFile


def is_valid(schema, attack):
    if schema != SCHEMA:
        return False
    if attack >= ATTACK_TOTAL:
        return False
    if attack in (ATTACK_DEBRUIJN, ATTACK_LOADFILE):
        return False
    return True


def main():
    ok = True

    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("OK  " if cond else "FAIL") + " " + msg)

    chk(is_valid(SCHEMA, 0), "schema 1, attack 0 -> valid")
    chk(is_valid(SCHEMA, 42), "schema 1, attack 42 (last normal) -> valid")
    chk(not is_valid(SCHEMA + 1, 0), "wrong schema -> invalid")
    chk(not is_valid(SCHEMA, ATTACK_TOTAL), "attack == total -> invalid")
    chk(not is_valid(SCHEMA, 99), "attack 99 -> invalid")
    chk(not is_valid(SCHEMA, ATTACK_DEBRUIJN), "DeBruijn (43) -> invalid")
    chk(not is_valid(SCHEMA, ATTACK_LOADFILE), "LoadFile (44) -> invalid")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
