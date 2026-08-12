#!/usr/bin/env bash
# Build the KeeLoq master-key host coverage matrix (Task 2) against the REAL
# lib/subghz/protocols/keeloq.c encoder+decoder and the REAL Task 1 registry
# (applications/main/subghz/helpers/subghz_keeloq_registry.c). Copy of
# build_lib.sh's compile recipe with the target .c/output swapped and the
# registry source added.
#   Run from repo root under WSL:
#     wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/keeloq_coverage_build.sh'
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

# components MUST precede lib (lib headers are #pragma-once redirects to bare
# <component/...> paths) — same ordering rule as build.sh / build_lib.sh.
INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib -Iapplications/main/subghz/helpers"
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)"

# Only what keeloq.c itself needs (+ its direct crypto/helper deps) — no other
# car protocols required for this coverage tool.
PROTOS="\
  $LP/keeloq.c \
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

# Task 1's registry (furi-free leaf) — classifies each keystore entry.
REGISTRY="applications/main/subghz/helpers/subghz_keeloq_registry.c"

HOST="$HT/keeloq_coverage.c $HT/host_shim.c $HT/keystore_host.c"

set -x
gcc -std=gnu11 -g -O1 -w -Wno-implicit-function-declaration \
  -Wno-error=implicit-function-declaration $INC $DEFS \
  $HOST $COMPAT $PROTOS $REGISTRY \
  -o "$HT/keeloq_coverage"
set +x
echo "BUILD OK -> $HT/keeloq_coverage"

# This tool's OWN source (not the vendored lib/components files, which stay
# -w above) must compile clean under -Wall -Wextra. Syntax-only recompile so
# this is reproducible from the committed script, not just a claim in a report.
set -x
gcc -std=gnu11 -c -Wall -Wextra -Wno-unused-parameter -fsyntax-only \
  $INC $DEFS "$HT/keeloq_coverage.c"
set +x
echo "keeloq_coverage.c: -Wall -Wextra clean"
