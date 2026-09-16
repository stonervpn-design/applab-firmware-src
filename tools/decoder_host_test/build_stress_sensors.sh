#!/usr/bin/env bash
# Build the SENSORS anti-phantom harness (stress_sensors.c): the 8 rtl_433 weather
# decoders + 22 extended TPMS/Schrader variants that are NOT in test_tpms_decode.
# Mirrors build_test_tpms.sh's include order + shim set.
#
# Run from repo root under WSL:  bash tools/decoder_host_test/build_stress_sensors.sh [caps.sub ...]
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)"

# 8 weather + 22 extended TPMS/Schrader + shared tpms_generic base.
DECODERS="acurite_tower lacrosse_tx141th lacrosse_tx nexus prologue oregon_thgr \
  fineoffset_wh2 ambient_weather \
  tpms_elantra2012 tpms_ave tpms_renault_0435r tpms_kia tpms_trw_fsk tpms_trw_ook \
  tpms_smartire tpms_jansite_ty468 tpms_jansite_ty588 tpms_mercedes_benz \
  tpms_airpuxem tpms_tyreguard400 tpms_gm tpms_eezrv tpms_schrader_motorcycle \
  tpms_imars_t240 tpms_sefis_m3 tpms_schraeder schrader_smd3ma4 schrader_nis315g3 \
  schrader_gg4 schrader_eg53ma4 tpms_generic"

DEC_SRC=""
for d in $DECODERS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done

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
  lib/subghz/protocols/protocols_common.c
"

set -x
gcc -std=gnu11 -g -O1 -w $INC $DEFS \
  -include $HT/host_hal_decls.h -include $HT/host_tpms_decls.h \
  $HT/stress_sensors.c $HT/host_shim.c $HT/host_shim_extra.c $HT/host_shim_tpms.c \
  $HT/keystore_host.c \
  $COMPAT components/datetime/datetime.c $DEC_SRC \
  -o "$HT/stress_sensors"
set +x
echo "BUILD OK -> $HT/stress_sensors"

if [ "$#" -ge 1 ]; then
    "$HT/stress_sensors" "$@"
fi
