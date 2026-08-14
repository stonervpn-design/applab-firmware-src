#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -DNRF24_EXPORT_HOST_TEST \
  "$HT/test_nrf24_export.c" \
  applications/main/nrf24/helpers/nrf24_export.c \
  -o "$HT/test_nrf24_export"
echo "BUILD OK"; "$HT/test_nrf24_export"
