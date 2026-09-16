#!/usr/bin/env python3
# Triage the corpus: for each .sub, measure capture "cleanliness" = the longest
# contiguous run of in-band pulses (a real keyfob frame is ~100+ pulses; noise
# fragments into tiny segments). Rank makes by their best clean frame so we know
# which NODECODE captures are worth building a coverage decoder from.
import os, glob, collections

ROOT = "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu"

def load(path):
    d=[]
    for line in open(path, encoding="utf-8", errors="ignore"):
        if line.startswith("RAW_Data:"):
            d += [int(x) for x in line.split(":",1)[1].split()]
    return d

def longest_frame(durs, gap=5000):
    best=cur=0
    for x in durs:
        if abs(x)>gap: cur=0
        else:
            cur+=1
            if cur>best: best=cur
    return best

rows=[]
for f in glob.glob(ROOT+"/**/*.sub", recursive=True):
    make = os.path.basename(os.path.dirname(f))
    d = load(f)
    if not d: continue
    lf = longest_frame(d)
    rows.append((make, os.path.basename(f), lf, len(d)))

# best clean-frame per make
bymake = collections.defaultdict(list)
for make,_,lf,_ in rows: bymake[make].append(lf)
print("=== per-make: max clean-frame pulses (higher = cleaner = more RE-able) ===")
for make in sorted(bymake, key=lambda m: -max(bymake[m])):
    v=bymake[make]
    print(f"  {make:32s} maxframe={max(v):4d}  median={sorted(v)[len(v)//2]:4d}  n={len(v)}")

print("\n=== cleanest individual captures (maxframe>=80, likely RE-able) ===")
clean=[r for r in rows if r[2]>=80]
for make,fn,lf,tot in sorted(clean, key=lambda r:-r[2])[:25]:
    print(f"  {lf:4d}  {make}/{fn}")
print(f"\nCLEAN captures (frame>=80 pulses): {len(clean)} of {len(rows)}")
