#!/usr/bin/env bash
# Re-test the files HondaV6 touched in the baseline run, after the 313-path fix.
set -e
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
HT=tools/decoder_host_test
BASE="$HT/_verify_decode_rt.txt"

awk -F'\t' '$3=="HondaV6"{print $1}' "$BASE" | sort -u > "$HT/_hondav6_files.txt"
echo "HondaV6-touched files (baseline): $(wc -l < "$HT/_hondav6_files.txt")"

# run rebuilt harness on just those files
./$HT/pp_decode_full $(cat "$HT/_hondav6_files.txt") 2>/dev/null > "$HT/_hondav6_rerun.txt"

echo "--- HondaV6 decode lines AFTER fix: $(awk -F'\t' '$3=="HondaV6"' "$HT/_hondav6_rerun.txt" | wc -l)"
echo "--- RT verdicts AFTER fix:"
awk -F'\t' '$3=="HondaV6"{print $5}' "$HT/_hondav6_rerun.txt" | sort | uniq -c
echo "--- remaining HondaV6 decode formats AFTER fix (313 vs 45bit):"
awk -F'\t' '$3=="HondaV6"{print $6}' "$HT/_hondav6_rerun.txt" | sed -E 's/(313MHz|[0-9]+bit).*/\1/' | sort | uniq -c
echo "--- full remaining HondaV6 lines (data):"
awk -F'\t' '$3=="HondaV6"{print $1"  ||  "$6}' "$HT/_hondav6_rerun.txt" | sed 's#.*/##'
