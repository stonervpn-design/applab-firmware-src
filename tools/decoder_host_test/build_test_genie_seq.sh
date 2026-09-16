#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
gcc -std=gnu11 -O2 -Wall -Wextra \
  -Iapplications/main/subghz/helpers \
  tools/decoder_host_test/test_genie_seq.c -o tools/decoder_host_test/test_genie_seq
echo "BUILD OK"; tools/decoder_host_test/test_genie_seq
