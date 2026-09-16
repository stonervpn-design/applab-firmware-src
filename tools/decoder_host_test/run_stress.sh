#!/usr/bin/env bash
# Run pp_decode_full over every .sub in $1 and report DECODED hits per file.
#   arg1 = directory of stress .sub files
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
DIR="$1"
BIN=tools/decoder_host_test/pp_decode_full
total=0
for f in "$DIR"/*.sub; do
  [ -f "$f" ] || continue
  hits=$("$BIN" "$f" 2>/dev/null | grep -c DECODED || true)
  n=$(PPDBG=1 "$BIN" "$f" 2>&1 1>/dev/null | grep -oE 'n=[0-9]+' | head -1)
  printf "%-18s %-10s DECODED=%s\n" "$(basename "$f")" "$n" "$hits"
  total=$((total + hits))
  if [ "$hits" -gt 0 ]; then
    echo "   --- phantom decodes ---"
    "$BIN" "$f" 2>/dev/null | grep DECODED | sed -E 's#^[^\t]*\t##; s/\t/  /g' | cut -c1-140 | sed 's/^/   /'
  fi
done
echo "TOTAL phantom decodes across all stress files: $total"
