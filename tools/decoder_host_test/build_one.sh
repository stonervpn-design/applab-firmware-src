#!/usr/bin/env bash
# Isolated single-protocol TPMS encoder round-trip build.
# Usage: bash tools/decoder_host_test/build_one.sh <proto_src_basename> <PROTO_SYMBOL> <PACK_SYMBOL> [id_hex pressure temp]
#   e.g. bash .../build_one.sh tpms_hyundai_vdo tpms_protocol_hyundai_vdo tpms_protocol_hyundai_vdo_pack 0C12ABCD 2.2 20
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test; LP=lib/subghz/protocols
PSRC="$1"; PROTO="$2"; PACK="$3"; shift 3 || true
OUT="$HT/test_one_${PSRC}"
INC="-I$HT/shim -I. -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents -Ilib"
COMPAT="components/furi/core/furi_string.c components/flipper_format/flipper_format.c \
  components/flipper_format/flipper_format_stream.c components/toolbox/stream/stream.c \
  components/toolbox/stream/string_stream.c components/toolbox/manchester_decoder.c \
  components/toolbox/manchester_encoder.c components/toolbox/hex.c components/toolbox/strint.c \
  lib/subghz/blocks/decoder.c lib/subghz/blocks/generic.c lib/subghz/blocks/math.c \
  lib/subghz/blocks/encoder.c lib/subghz/blocks/custom_btn.c lib/subghz/protocols/base.c \
  components/datetime/datetime.c $LP/tpms_generic.c"
gcc -std=gnu11 -g -O1 -w $INC -D_ATTRIBUTE\(x\)=__attribute__\(x\) \
  "-DONE_PROTO=$PROTO" "-DPACK=$PACK" \
  -include $HT/host_hal_decls.h -include $HT/host_tpms_decls.h \
  $HT/test_tpms_one.c $HT/host_shim.c $HT/host_shim_extra.c $HT/host_shim_tpms.c $HT/keystore_host.c \
  $COMPAT $LP/$PSRC.c -o "$OUT"
echo "BUILD OK -> $OUT"
"$OUT" "$@"
