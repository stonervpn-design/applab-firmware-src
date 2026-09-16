#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
INC="-I$HT/shim -I. -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents -Ilib"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x) -DENABLE_EMULATE_FEATURE"
COMPAT="components/furi/core/furi_string.c components/flipper_format/flipper_format.c \
  components/flipper_format/flipper_format_stream.c components/toolbox/stream/stream.c \
  components/toolbox/stream/string_stream.c components/toolbox/manchester_decoder.c \
  components/toolbox/manchester_encoder.c components/toolbox/hex.c components/toolbox/strint.c \
  lib/subghz/blocks/decoder.c lib/subghz/blocks/generic.c lib/subghz/blocks/math.c \
  lib/subghz/blocks/encoder.c lib/subghz/protocols/base.c $HT/host_shim.c"
gcc -std=gnu11 -g -O1 -w $INC $DEFS -include $HT/host_hal_decls.h \
  $HT/test_peugot_roundtrip.c $COMPAT -o "$HT/test_peugot_roundtrip"
echo "BUILD OK"; "$HT/test_peugot_roundtrip"
