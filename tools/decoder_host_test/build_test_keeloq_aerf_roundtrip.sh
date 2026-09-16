#!/usr/bin/env bash
# Build+run the AERF firmware-source round-trip proof (compiles the REAL
# lib/subghz/protocols/keeloq_common.c, not a copy). Run from repo root under WSL:
#   bash tools/decoder_host_test/build_test_keeloq_aerf_roundtrip.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents -Ilib"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)"

set -x
gcc -std=gnu11 -g -O2 -w $INC $DEFS \
  $HT/test_keeloq_aerf_roundtrip.c $LP/keeloq_common.c \
  -o "$HT/test_keeloq_aerf_roundtrip"
set +x
echo "BUILD OK -> $HT/test_keeloq_aerf_roundtrip"
"$HT/test_keeloq_aerf_roundtrip"
