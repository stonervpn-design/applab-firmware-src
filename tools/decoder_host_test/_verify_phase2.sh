#!/usr/bin/env bash
# Phase 2: rebuild harness with the honda_v6 313-path fix, re-check HondaV6,
# then run the counter/desync (SYNTHMONO + SYNTHCHECK) pass on decoded files.
set -e
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
HT=tools/decoder_host_test

echo "===== REBUILD (EMU=1) ====="
EMU=1 bash "$HT/build_full.sh" >/dev/null 2>&1
echo "rebuilt OK"

echo "===== HONDAV6 RE-CHECK (after overran gate) ====="
./$HT/pp_decode_full $(cat "$HT/_hondav6_files.txt") 2>/dev/null > "$HT/_hondav6_rerun.txt"
echo "HondaV6 decode lines now: $(awk -F'\t' '$3=="HondaV6"' "$HT/_hondav6_rerun.txt" | wc -l)"
awk -F'\t' '$3=="HondaV6"{print $5}' "$HT/_hondav6_rerun.txt" | sort | uniq -c
echo "remaining HondaV6 formats:"
awk -F'\t' '$3=="HondaV6"{print $6}' "$HT/_hondav6_rerun.txt" | sed -E 's/(313MHz|[0-9]+bit).*/\1/' | sort | uniq -c

echo "===== COUNTER / DESYNC PASS (SYNTHMONO + SYNTHCHECK) ====="
SYNTHMONO=1 SYNTHCHECK=1 ./$HT/pp_decode_full $(cat "$HT/_decoded_files.txt") 2>/dev/null > "$HT/_verify_counters.txt"
echo "counter-pass decode lines: $(grep -c $'\tDECODED\t' "$HT/_verify_counters.txt")"
echo "--- MONO verdicts (per decode) ---"
grep -oE 'MONO:[A-Za-z/()_+-]+' "$HT/_verify_counters.txt" | sort | uniq -c | sort -rn
echo "--- SYNTH verdicts (per decode) ---"
grep -oE 'SYNTH:[A-Za-z/()_+-]+' "$HT/_verify_counters.txt" | sort | uniq -c | sort -rn
echo "===== PHASE2 DONE ====="
