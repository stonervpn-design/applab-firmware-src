#!/usr/bin/env bash
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3
for f in Suzuki_Isuzu/LDV_T80_lock Suzuki_Isuzu/LDV_T80_unlock; do
  tools/decoder_host_test/pp_decode_full "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/Asian/$f.sub" 2>/dev/null | grep -E "LDV V0"
done
