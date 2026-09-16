#!/usr/bin/env bash
# Build+run the AUT64 host KAT. aut64.c is furi-free (only <string.h>), so this
# is a trivial standalone compile — no shim/flipper_format needed.
#   Run from repo root under WSL:
#     wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_aut64_kat.sh'
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols
OUT="$HT/test_aut64_kat"

cc -O2 -std=c11 -Wall -Wextra \
  -DAUT64_ENABLE_VALIDATIONS \
  "$HT/test_aut64_kat.c" "$LP/aut64.c" \
  -o "$OUT"
echo "built $OUT"
"$OUT"
