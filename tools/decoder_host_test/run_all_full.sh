#!/usr/bin/env bash
# Full-registry round-trip corpus sweep. Runs pp_decode_full (built with EMU=1) with
# SYNTHMONO=1 over a .sub corpus and tallies decode + round-trip (RT) + monotonic (MONO)
# verdicts per protocol. Usage: bash tools/decoder_host_test/run_all_full.sh [corpus_dir]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/tools/decoder_host_test/pp_decode_full"
CORPUS="${1:-/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu}"
OUT="${OUT:-/tmp/rt_sweep.txt}"
: > "$OUT"
[ -x "$BIN" ] || { echo "build first: EMU=1 bash tools/decoder_host_test/build_full.sh"; exit 1; }
count=0
while IFS= read -r f; do
  count=$((count + 1))
  SYNTHMONO=1 "$BIN" "$f" 2>/dev/null >> "$OUT"
done < <(find "$CORPUS" -iname '*.sub' 2>/dev/null)
echo "scanned $count .sub files -> $OUT"
echo "=== decode hits: $(grep -c 'DECODED' "$OUT") ==="
echo "=== RT verdict tally ==="
grep -oE 'RT:[A-Za-z_-]+' "$OUT" | sort | uniq -c | sort -rn
echo "=== MONO verdict tally ==="
grep -oE 'MONO:[A-Za-z_-]+' "$OUT" | sort | uniq -c | sort -rn
echo "=== protocols with >=1 RT:EXACT (decode->encode->decode byte-identical) ==="
grep 'RT:EXACT' "$OUT" | cut -f3 | sort -u
echo "=== protocols with any RT:FAIL (investigate) ==="
grep -E 'RT:FAIL' "$OUT" | cut -f3 | sort -u
