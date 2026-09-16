#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols
INC="-I$HT/shim -I. -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents -Ilib -Iapplications/main/subghz/helpers"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x) -DENABLE_EMULATE_FEATURE"
DEPS="$LP/protocols_common.c components/furi/core/furi_string.c components/flipper_format/flipper_format.c components/flipper_format/flipper_format_stream.c components/toolbox/stream/stream.c components/toolbox/stream/string_stream.c components/toolbox/manchester_decoder.c components/toolbox/hex.c components/toolbox/strint.c lib/subghz/blocks/decoder.c lib/subghz/blocks/generic.c lib/subghz/blocks/math.c lib/subghz/blocks/encoder.c lib/subghz/blocks/custom_btn.c lib/subghz/protocols/base.c $HT/host_shim.c"
gcc -std=gnu11 -g -O1 -w -Wno-implicit-function-declaration $INC $DEFS $HT/test_mazda_v0_rt.c $DEPS -o "$HT/test_mazda_v0_rt"
echo "BUILD OK -> $HT/test_mazda_v0_rt"
