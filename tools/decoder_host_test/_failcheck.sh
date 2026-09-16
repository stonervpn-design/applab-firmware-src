#!/usr/bin/env bash
echo "=== remaining RT:FAIL lines ==="
grep -E "RT:FAIL" /tmp/rt_sweep.txt | sed -E 's/\|Yek:[^|]*//' | cut -f3,5 | cut -c1-90 | sort -u
echo "=== LDV V0 + Peugot V0 verdicts now ==="
grep -E "LDV V0|Peugot V0" /tmp/rt_sweep.txt | cut -f3,5 | cut -c1-60 | sort -u
