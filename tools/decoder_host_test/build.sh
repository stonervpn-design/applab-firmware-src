#!/usr/bin/env bash
# Build the host decode/encode harness against the REAL firmware decoders.
# Run from repo root under WSL:  bash tools/decoder_host_test/build.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
STAGE=$HT/stage
PP=applications_user/proto_pirate

# ---- stage the real decoder sources against a lightweight app_i.h shim ----
rm -rf "$STAGE"
mkdir -p "$STAGE/protocols"
cp "$PP"/protocols/*.c "$STAGE/protocols/" 2>/dev/null || true
cp "$PP"/protocols/*.h "$STAGE/protocols/" 2>/dev/null || true
cp "$PP"/defines.h "$STAGE/" 2>/dev/null || true
cat > "$STAGE/protopirate_app_i.h" <<'EOF'
#pragma once
// Harness shim: decoders are app-independent; give them only the protocol contract.
#include "defines.h"
#include "protocols/protocols_common.h"
EOF

# Components MUST precede lib: the lib/ headers are #pragma-once redirects to bare
# <component/...> paths, so components has to win the bare lookup (else the redirect
# resolves back to itself and expands empty).
INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib"
# newlib macros the ESP32 toolchain provides but host glibc does not:
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)"

# Which decoder .c to compile. Override: DECODERS="a b c"
DECODERS="${DECODERS:-\
  chrysler_v0 fiat_v0 fiat_v1 fiat_v2 ford_v0 ford_v1 ford_v2 ford_v3 \
  honda_static honda_v1 honda_v2 kia_v0 kia_v1 kia_v2 kia_v3_v4 kia_v5 kia_v6 kia_v7 \
  mazda_v0 mitsubishi_v0 porsche_touareg psa renault_v0 scher_khan star_line subaru vag \
  protocols_common protocol_timings keys aut64 psa_crypto psa_bf_core psa_crypto_bf}"
DEC_SRC=""
for d in $DECODERS; do DEC_SRC="$DEC_SRC $STAGE/protocols/$d.c"; done

COMPAT="
  components/furi/core/furi_string.c
  components/flipper_format/flipper_format.c
  components/flipper_format/flipper_format_stream.c
  components/toolbox/stream/stream.c
  components/toolbox/stream/string_stream.c
  components/toolbox/manchester_decoder.c
  components/toolbox/hex.c
  components/toolbox/strint.c
  lib/subghz/blocks/decoder.c
  lib/subghz/blocks/generic.c
  lib/subghz/blocks/math.c
  lib/subghz/blocks/encoder.c
  lib/subghz/protocols/base.c
"

set -x
gcc -std=gnu11 -g -O1 -w $INC $DEFS \
  $HT/pp_decode.c $HT/host_shim.c $HT/keystore_host.c \
  $COMPAT $DEC_SRC \
  -o "$HT/pp_decode"
set +x
echo "BUILD OK -> $HT/pp_decode"
