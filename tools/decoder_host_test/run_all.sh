#!/usr/bin/env bash
# Run every .sub in the automotive collection through the real firmware decoders.
# Paths hardcoded (WSL) to avoid Git-Bash->wsl arg mangling.
# Invoke:  wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/run_all.sh'
set -u
BIN=tools/decoder_host_test/pp_decode
COLL="/mnt/c/Users/stone/Downloads/auto_subghz_test/Automotive-Sub-Ghz-Collection-main"
OUT="/mnt/c/Users/stone/Downloads/auto_subghz_test/decode_results.tsv"

: > "$OUT"
total=0
dec=0
while IFS= read -r f; do
    total=$((total + 1))
    rel="${f#"$COLL"/}"
    res="$("$BIN" "$f" 2>/dev/null)"
    if printf '%s' "$res" | grep -q '	DECODED	'; then
        dec=$((dec + 1))
        printf '%s\n' "$res" | sed "s#$COLL/##" >> "$OUT"
    else
        printf '%s\tNODECODE\n' "$rel" >> "$OUT"
    fi
done < <(find "$COLL" -name '*.sub' | sort)

echo "TOTAL=$total  DECODED=$dec  NODECODE=$((total - dec))"
echo "--- per protocol ---"
grep '	DECODED	' "$OUT" | awk -F'\t' '{print $3}' | sort | uniq -c | sort -rn
echo "--- decoded by brand ---"
grep '	DECODED	' "$OUT" | awk -F'\t' '{print $1}' | awk -F/ '{print $1"/"$2}' | sort | uniq -c | sort -rn
