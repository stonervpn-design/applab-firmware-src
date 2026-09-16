#!/usr/bin/env bash
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
tools/decoder_host_test/pp_decode_full "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/European/Groupe_PSA/Peugeot_307_2001_2008_open.sub" 2>/dev/null | grep -E "Peugot V0"
