#!/usr/bin/env bash
# Counter/desync verification: SYNTHMONO (monotonic lockstep proof) + SYNTHCHECK
# on all decoded captures, using the harness rebuilt with the honda_v6 fix.
set -e
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
HT=tools/decoder_host_test
SYNTHMONO=1 SYNTHCHECK=1 ./$HT/pp_decode_full $(cat "$HT/_decoded_files.txt") 2>/dev/null > "$HT/_verify_counters.txt"
echo "===== COUNTER PASS DONE ====="
echo "decode lines: $(grep -c $'\tDECODED\t' "$HT/_verify_counters.txt")"
echo "--- MONO verdicts (rolling-counter desync-safety) ---"
grep -oE 'MONO:[A-Za-z/()_+.-]+' "$HT/_verify_counters.txt" | sort | uniq -c | sort -rn
echo "--- SYNTH verdicts ---"
grep -oE 'SYNTH:[A-Za-z/()_+.-]+' "$HT/_verify_counters.txt" | sort | uniq -c | sort -rn
echo "--- MONO:FAIL detail (protocol : detail) ---"
awk -F'\t' '/MONO:FAIL/{proto=$3; for(i=1;i<=NF;i++) if($i ~ /MONO:/) print proto" : "$i}' "$HT/_verify_counters.txt" | sort | uniq -c
