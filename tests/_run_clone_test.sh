#!/bin/bash
set -e
cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3/tests
gcc -O1 -Wall -Wextra -o /tmp/tkc test_keeloq_clone_core.c
/tmp/tkc
echo "EXIT=$?"
