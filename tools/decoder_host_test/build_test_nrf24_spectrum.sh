#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_spectrum.c" \
  applications/main/nrf24/helpers/nrf24_spectrum_calc.c \
  -o "$HT/test_nrf24_spectrum"
echo "BUILD OK"; "$HT/test_nrf24_spectrum"
