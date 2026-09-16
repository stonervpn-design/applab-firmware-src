#!/usr/bin/env bash
# Build the TPMS decode validation harness (test_tpms_decode.c) against the REAL
# firmware lib/subghz TPMS decoders. Mirrors build_full.sh's include order + shim
# set, but compiles the tpms_*.c decoders + tpms_generic.c instead of the car set.
#
# Run from repo root under WSL:  bash tools/decoder_host_test/build_test_tpms.sh [caps.sub ...]
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)${FORDTRACE:+ -DFORD_TRACE}"

# 17 TPMS decoders + their shared base (tpms_generic).
DECODERS="tpms_ford tpms_toyota tpms_pmv107j tpms_honda tpms_nissan \
  tpms_hyundai_vdo tpms_hyundai_ev tpms_renault tpms_citroen tpms_abarth124 tpms_jansite \
  tpms_jeep tpms_truck tpms_porsche tpms_bmw tpms_bmw_g3 \
  schrader_eg53ma4 schrader_gg4 tpms_generic"

DEC_SRC=""
for d in $DECODERS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done

# Same COMPAT block as build_full.sh (furi_string / flipper_format / stream /
# manchester / blocks / base) — the TPMS decoders link the manchester decoder +
# the block helpers + base callback.
COMPAT="
  components/furi/core/furi_string.c
  components/flipper_format/flipper_format.c
  components/flipper_format/flipper_format_stream.c
  components/toolbox/stream/stream.c
  components/toolbox/stream/string_stream.c
  components/toolbox/manchester_decoder.c
  components/toolbox/manchester_encoder.c
  components/toolbox/hex.c
  components/toolbox/strint.c
  lib/subghz/blocks/decoder.c
  lib/subghz/blocks/generic.c
  lib/subghz/blocks/math.c
  lib/subghz/blocks/encoder.c
  lib/subghz/blocks/custom_btn.c
  lib/subghz/protocols/base.c
"

set -x
gcc -std=gnu11 -g -O1 -w $INC $DEFS \
  -include $HT/host_hal_decls.h -include $HT/host_tpms_decls.h \
  $HT/test_tpms_decode.c $HT/host_shim.c $HT/host_shim_extra.c $HT/host_shim_tpms.c \
  $HT/keystore_host.c \
  $COMPAT components/datetime/datetime.c $DEC_SRC \
  -o "$HT/test_tpms_decode"
set +x
echo "BUILD OK -> $HT/test_tpms_decode"

# If capture args were passed, run them; else run the bundled real Ford capture.
if [ "$#" -ge 1 ]; then
    "$HT/test_tpms_decode" "$@"
fi
