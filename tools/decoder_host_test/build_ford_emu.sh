#!/usr/bin/env bash
# Build the per-protocol synth->decode round-trip sweep against the REAL
# lib/subghz/protocols car encoders/decoders (NOT proto_pirate). The lib
# protocols include their own "protocols_common.h" (a lightweight contract),
# so — unlike build.sh — NO app_i.h staging shim is needed; we compile the
# lib .c files in place.
#   Run from repo root under WSL:
#     wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_lib.sh'
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

# components MUST precede lib (lib headers are #pragma-once redirects to bare
# <component/...> paths) — same ordering rule as build.sh.
INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib -Iapplications/main/subghz/helpers"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x) -DENABLE_EMULATE_FEATURE"

# The 21 SUPPORTED car protocols (Task 1) + their direct crypto/helper deps.
PROTOS="\
  $LP/vag.c $LP/keeloq.c $LP/kia_v0.c $LP/kia_v1.c $LP/kia_v7.c \
  $LP/hyundai_kia_rio.c $LP/santa_fe.c $LP/ford_v0.c $LP/ford_v1.c $LP/ford_v3.c \
  $LP/honda_static.c $LP/honda_v2_v3.c $LP/honda_v5.c $LP/honda_v7.c $LP/honda_v8.c \
  $LP/honda_v9.c $LP/mazda_v0.c $LP/subaru.c $LP/fiat_v1.c $LP/chrysler_v0.c \
  $LP/land_rover_v0.c \
  $LP/protocols_common.c $LP/keeloq_common.c $LP/aut64.c \
  $LP/subghz_keeloq_clone_core.c $LP/hitag2_cipher.c $LP/keys.c $LP/aes_common.c"

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
  lib/subghz/protocols/base.c"

HOST="$HT/test_ford_emulate.c $HT/host_shim.c $HT/keystore_host.c \
  applications/main/subghz/helpers/subghz_fob_buttons.c"

set -x
# furi_hal_subghz_get_rolling_counter_mult is provided by host_shim.c but the
# protocol .c files carry no visible prototype; keep implicit-decl a warning.
gcc -std=gnu11 -g -O1 -w -Wno-implicit-function-declaration \
  -Wno-error=implicit-function-declaration $INC $DEFS \
  $HOST $COMPAT $PROTOS \
  -o "$HT/test_ford_emulate"
set +x
echo "BUILD OK -> $HT/test_ford_emulate"
