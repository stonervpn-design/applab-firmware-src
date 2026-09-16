#!/usr/bin/env bash
set -e
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
HT=tools/decoder_host_test
./$HT/pp_decode_full /mnt/c/Users/stone/All_SubGHz/*.sub 2>/dev/null > "$HT/_verify_decode_rt_after.txt"
echo "===== RESWEEP DONE ====="
echo "total DECODED: $(grep -c $'\tDECODED\t' "$HT/_verify_decode_rt_after.txt")"
echo "--- RT verdicts AFTER ---"
awk -F'\t' '/\tDECODED\t/{print $5}' "$HT/_verify_decode_rt_after.txt" | sort | uniq -c | sort -rn
echo "--- HondaV6 decodes AFTER (full corpus) ---"
awk -F'\t' '$3=="HondaV6"{print $5}' "$HT/_verify_decode_rt_after.txt" | sort | uniq -c
echo "--- per-protocol decode counts AFTER ---"
awk -F'\t' '/\tDECODED\t/{print $3}' "$HT/_verify_decode_rt_after.txt" | sort | uniq -c | sort -rn
