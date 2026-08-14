#!/usr/bin/env bash
# Build+run the land_rover_rke host test (plan B, Task 1). Mirrors build_lib.sh's
# compile recipe, target swapped to test_land_rover_rke.c.
#   Run from repo root under WSL:
#     wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_land_rover_rke.sh'
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib -Iapplications/main/subghz/helpers"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)"

SRC="$HT/test_land_rover_rke.c \
  $LP/land_rover_rke.c $LP/protocols_common.c"

COMPAT="\
  components/furi/core/furi_string.c \
  components/flipper_format/flipper_format.c \
  components/flipper_format/flipper_format_stream.c \
  components/toolbox/stream/stream.c \
  components/toolbox/stream/string_stream.c \
  components/toolbox/manchester_decoder.c \
  components/toolbox/hex.c \
  components/toolbox/strint.c \
  lib/subghz/blocks/decoder.c \
  lib/subghz/blocks/generic.c \
  lib/subghz/blocks/math.c \
  lib/subghz/blocks/encoder.c \
  lib/subghz/blocks/custom_btn.c \
  lib/subghz/protocols/base.c \
  $HT/host_shim.c"

set -x
gcc -std=gnu11 -g -O1 -w $INC $DEFS $SRC $COMPAT -o "$HT/test_land_rover_rke"
set +x
echo "BUILD OK -> $HT/test_land_rover_rke"
"$HT/test_land_rover_rke"
