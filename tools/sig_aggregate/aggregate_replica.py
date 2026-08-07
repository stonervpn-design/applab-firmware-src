#!/usr/bin/env python3
"""Validation replica of subghz_sig_aggregate.c (multi-frame K-of-N vote).
No local gcc -> this Python mirror is the executable check. Keep in step with the C."""
import sys


def aggregate(frames, min_votes, min_score):
    # frames: list of {"name":..., "score":...}; returns the winning frame or UNKNOWN.
    tally = {}  # name -> [count, best_score, best_idx]
    for i, f in enumerate(frames):
        name, score = f.get("name"), f.get("score", 0)
        if not name or name == "UNKNOWN" or score < min_score:
            continue
        if name not in tally:
            tally[name] = [0, -1, i]
        tally[name][0] += 1
        if score > tally[name][1]:
            tally[name][1] = score
            tally[name][2] = i
    if not tally:
        return {"name": "UNKNOWN"}
    # winner: most votes, ties broken by higher best_score
    name = max(tally, key=lambda k: (tally[k][0], tally[k][1]))
    count, _bs, best_idx = tally[name]
    if count < min_votes:
        return {"name": "UNKNOWN"}
    return frames[best_idx]


def main():
    ok = True

    def chk(cond, msg):
        nonlocal ok
        ok = ok and cond
        print(("OK  " if cond else "FAIL") + " " + msg)

    F = lambda n, s: {"name": n, "score": s}

    r = aggregate([F("Kia V7", 60), F("Kia V7", 75), F("Kia V7", 55)], 2, 40)
    chk(r["name"] == "Kia V7" and r["score"] == 75, "3 agree -> best frame")
    r = aggregate([F("Kia V7", 60), F("Ford V0", 50), F("Kia V7", 65)], 2, 40)
    chk(r["name"] == "Kia V7" and r["score"] == 65, "2 of 3 agree")
    r = aggregate([F("Kia V7", 60), F("Ford V0", 60), F("Honda", 60)], 2, 40)
    chk(r["name"] == "UNKNOWN", "all distinct -> UNKNOWN")
    r = aggregate([F("Kia V7", 30), F("Kia V7", 35), F("Kia V7", 20)], 2, 40)
    chk(r["name"] == "UNKNOWN", "below floor -> UNKNOWN")
    r = aggregate([F("Kia V7", 60), F("UNKNOWN", 0), F("Kia V7", 70)], 2, 40)
    chk(r["name"] == "Kia V7" and r["score"] == 70, "2 valid + 1 UNKNOWN")
    r = aggregate([], 2, 40)
    chk(r["name"] == "UNKNOWN", "empty -> UNKNOWN")

    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
