#!/usr/bin/env bash
# Build+run the toyota_keeloq host test (plan B, Task 2). Mirrors build_lib.sh's
# compile recipe, target swapped to test_toyota_keeloq.c.
#   Run from repo root under WSL:
#     wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_toyota_keeloq.sh'
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

SRC="$HT/test_toyota_keeloq.c \
  $LP/toyota_keeloq.c $LP/protocols_common.c"

# toyota_keeloq.c now performs KeeLoq manufacturer-key recovery, so it links the
# real KeeLoq crypto (keeloq_common.c) + a host keystore shim (keystore_host.c,
# which loads the plaintext keeloq_mfcodes_user asset incl. the Toyota entry).
SRC="$SRC $LP/keeloq_common.c $HT/keystore_host.c"

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
gcc -std=gnu11 -g -O1 -w $INC $DEFS $SRC $COMPAT -o "$HT/test_toyota_keeloq"
set +x
echo "BUILD OK -> $HT/test_toyota_keeloq"
"$HT/test_toyota_keeloq"
