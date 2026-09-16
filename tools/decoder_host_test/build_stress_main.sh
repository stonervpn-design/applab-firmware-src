#!/usr/bin/env bash
# Build the MAIN/standard-OOK anti-phantom harness (stress_main.c): every
# non-TPMS, non-RAW decoder in subghz_protocol_registry_items[]. Mirrors
# build_full.sh's include order + shim set.
#
# Run from repo root under WSL:  bash tools/decoder_host_test/build_stress_main.sh [caps.sub ...]
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

# The main-registry standard-OOK decoder .c files (file basenames).
DECODERS="gate_tx keeloq nice_flo came faac_slh nice_flor_s came_twee came_atomo \
  nero_sketch ido hormann nero_radio somfy_telis somfy_keytis princeton retekess \
  linear secplus_v2 secplus_v1 megacode holtek chamberlain_code power_smart \
  marantec bett doitrand phoenix_v2 honeywell_wdb magellan intertechno_v3 clemsa \
  ansonic smc5326 holtek_ht12x linear_delta3 dooya alutech_at_4n kinggates_stylo_4k \
  mastercode honeywell legrand dickert_mahs gangqi marantec24 hollarm hay21 \
  revers_rb2 feron roger elplast treadmill37 beninca_arc jarolift ditec_gol4 \
  keyfinder telcoma_edge allstar_firefly nord_ice x10 hormann_bisecur cardin_s508 \
  pocsag genie tesla_chargeport"

# Shared crypto/helper deps the standard decoders link against (non-decoder .c).
DEPS="protocols_common keys keeloq_common subghz_keeloq_clone_core aes_common pcsg_generic"

DEC_SRC=""
for d in $DECODERS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done
for d in $DEPS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done

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
  $HT/stress_main.c $HT/host_shim.c $HT/host_shim_extra.c $HT/host_shim_tpms.c \
  $HT/host_shim_main.c $HT/keystore_host.c \
  $COMPAT components/datetime/datetime.c $DEC_SRC \
  -lm \
  -o "$HT/stress_main"
set +x
echo "BUILD OK -> $HT/stress_main"

if [ "$#" -ge 1 ]; then
    "$HT/stress_main" "$@"
fi
