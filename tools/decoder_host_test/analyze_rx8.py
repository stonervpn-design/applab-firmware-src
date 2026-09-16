#!/usr/bin/env python3
# Characterize the Mazda RX8 2007 keyfob protocol from the real .sub captures.
import sys, glob, os, collections

MDIR_CANDS = [
    "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/Asian/Mazda",
    "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/European/Mazda",
]
mdir = next((d for d in MDIR_CANDS if os.path.isdir(d)), None)
if not mdir:
    print("no Mazda dir found"); sys.exit(1)

def load_raw(path):
    durs = []
    freq = preset = "?"
    for line in open(path, encoding="utf-8", errors="ignore"):
        if line.startswith("Frequency:"): freq = line.split(":",1)[1].strip()
        elif line.startswith("Preset:"): preset = line.split(":",1)[1].strip()
        elif line.startswith("RAW_Data:"):
            durs += [int(x) for x in line.split(":",1)[1].split()]
    return durs, freq, preset

def frames(durs, gap=5000):
    """Split into segments on |dur|>gap. Return list of segments (lists of durs)."""
    segs, cur = [], []
    for d in durs:
        if abs(d) > gap:
            if len(cur) >= 8: segs.append(cur)
            cur = []
        else:
            cur.append(d)
    if len(cur) >= 8: segs.append(cur)
    return segs

def hist(vals, edges):
    c = collections.Counter()
    for v in vals:
        a = abs(v)
        for lo,hi,name in edges:
            if lo <= a < hi: c[name]+=1; break
    return c

EDGES = [(0,180,"~130"),(180,360,"~260"),(360,650,"~500"),(650,1100,"~800"),
         (1100,2000,"~1700"),(2000,5000,"~2-5k"),(5000,10**9,">5k")]

f = os.path.join(mdir, "Mazda_RX8_2007_10x_lock.sub")
durs, freq, preset = load_raw(f)
print(f"file={os.path.basename(f)} freq={freq} preset={preset} total_durs={len(durs)}")
print("timing histogram:", dict(hist(durs, EDGES)))
segs = frames(durs)
sizes = sorted((len(s) for s in segs), reverse=True)
print(f"frame segments (>{5000}us gaps): count={len(segs)} sizes(top)={sizes[:12]}")
if segs:
    big = max(segs, key=len)
    print(f"\n=== longest frame ({len(big)} pulses) ===")
    print(" ".join(str(d) for d in big[:120]))
    print("\nframe timing histogram:", dict(hist(big, EDGES)))
