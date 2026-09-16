#!/usr/bin/env bash
# Build the FULL host decode harness against the REAL firmware lib/subghz
# automotive decoders -- the SAME set the DEVICE registers in
# lib/subghz/protocols/protocol_items.c (subghz_protocol_registry_items_automotive[],
# #ifndef PUBLIC_BUILD), minus raw/bin_raw.
#
# SEPARATE from build.sh / pp_decode (which tests only the 27 proto_pirate copies).
# Single tree only: compiles lib/subghz/protocols/*.c decoders directly -- NO
# proto_pirate copies (mixing trees duplicates shared helpers -> link errors).
#
# Run from repo root under WSL:  bash tools/decoder_host_test/build_full.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HT=tools/decoder_host_test
LP=lib/subghz/protocols

# Components MUST precede lib: the lib/ headers are #pragma-once redirects to bare
# <component/...> paths, so components has to win the bare lookup (else the redirect
# resolves back to itself and expands empty). Same include order as build.sh.
INC="-I$HT/shim -I. \
  -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
  -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
  -Ilib"
# newlib macro the ESP32 toolchain provides but host glibc does not (same as build.sh).
# shim/esp_heap_caps.h stubs the PSRAM allocator so the aes_common -> subghz_keystore.h ->
# <esp_heap_caps.h> chain builds hermetically on the host.
#
# EMU=1 adds -DENABLE_EMULATE_FEATURE, which compiles the encoders (needed for pp_decode_full's
# round-trip/synth checks — without it every encoder vtable is NULL and those checks silently
# no-op). NOTE: turning encoders on currently surfaces latent per-decoder encoder compile gaps
# in the LIB harness context (e.g. hyundai_tl.c's ENABLE_EMULATE_FEATURE struct is used without a
# visible typedef here; several encoders were authored against proto_pirate's pp_* helpers). Those
# are per-decoder fixes; until they land, run the decode-only sweep (default) or use the focused
# KeeLoq round-trip harness (build_test_keeloq_roundtrip.sh) which DOES compile the real encoder.
DEFS="-D_ATTRIBUTE(x)=__attribute__(x)${EMU:+ -DENABLE_EMULATE_FEATURE}"

# ---- the 63 automotive decoder .c files = EXACTLY protocol_items.c's automotive
# array (minus raw/bin_raw), in file order. Override: DECODERS="a b c"
DECODERS="${DECODERS:-\
  bmw_cas4 hyundai_kia_rio hyundai_tl mazda_siemens sheriff_cfm fiat_v1 \
  chrysler_v0 fiat_v0 ford_v0 ford_v1 ford_v2 ford_v3 honda_static \
  kia_v0 kia_v1 kia_v2 kia_v3_v4 kia_v5 kia_v6 kia_v7 \
  land_rover_v0 land_rover_rke bmw_ook mazda_v0 mitsubishi_v0 porsche_touareg psa \
  scher_khan star_line subaru vag audi_v1 audi_v2 audi_v3 bmw_v2 bmw_v3 \
  chevrolet_v0 ducato_v0 fiat_spa fiat_v2_v3_v5 hitag2 \
  honda_v2_v3 honda_v4 honda_v5 honda_v6 honda_v7 honda_v8 honda_v9 honda_v10 \
  jeep_v0 ldv_v0 lexus_v0 mazda_v1 mazda_v2 mitsubishi_v1 nissan_v0 opel_v0 \
  peugot_v0 renault_v1 renault_v2 renault_v3 renault_v4 toyota_v0 toyota_keeloq \
  compustar astrostart audiovox gm_rke continental_rke nidec_rke alps_rke vag_polo \
  retekess}"

# ---- shared crypto/helper deps the decoders link against (non-decoder .c) ----
# protocol_timings.c does NOT exist in lib/ (it was a proto_pirate-only file), so
# it is intentionally omitted; lib decoders carry their timings inline.
DEPS="protocols_common keys keeloq_common aut64 \
  psa_crypto psa_crypto_bf psa_bf_core hitag2_cipher aes_common"

DEC_SRC=""
for d in $DECODERS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done
for d in $DEPS; do DEC_SRC="$DEC_SRC $LP/$d.c"; done

# Same COMPAT block as build.sh (furi_string / flipper_format / stream / blocks / base).
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
gcc -std=gnu11 -g -O1 -w $INC $DEFS -include $HT/host_hal_decls.h \
  $HT/pp_decode_full.c $HT/host_shim.c $HT/host_shim_extra.c $HT/keystore_host.c \
  $COMPAT $DEC_SRC \
  -o "$HT/pp_decode_full"
set +x
echo "BUILD OK -> $HT/pp_decode_full"

# ---- sample run: proves keystore env + a decoder fire (expect a Kia V5 line) ----
SAMPLE="/mnt/c/Users/stone/All_SubGHz/Kia_V5_N1_RAW.sub"
echo "=== sample run: $SAMPLE ==="
"$HT/pp_decode_full" "$SAMPLE"
