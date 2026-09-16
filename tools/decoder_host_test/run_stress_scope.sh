#!/usr/bin/env bash
# Run one anti-phantom harness ($1) over the full stress corpus + the 2 real
# captures, with LITERAL paths (avoids the wsl -c inline var-clobber quirk).
#   arg1 = harness binary basename in tools/decoder_host_test/ (e.g. stress_sensors)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
BIN="tools/decoder_host_test/$1"
STRESS="/mnt/c/Users/stone/AppData/Local/Temp/claude/C--Windows-System32/99f61c0b-2898-40f2-bb2d-7777df4e34b1/scratchpad/stress"
REAL1="/mnt/c/Users/stone/Downloads/RAW_kurwa_anomalija.sub"
REAL2="/mnt/c/Users/stone/Downloads/RAW_unknown_bobr.sub"
echo "=== $1 over $(ls "$STRESS"/*.sub | wc -l) stress files + 2 real captures ==="
"$BIN" "$STRESS"/*.sub "$REAL1" "$REAL2" 2>/dev/null
