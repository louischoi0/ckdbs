#!/usr/bin/env bash
# The K sweep: reservations per transaction, one fresh server pair per K.
set -euo pipefail
W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2
for k in 1 2 4 8 16 32; do
    echo "=== K=$k ==="
    "$W/bench/run_abort_sweep.sh" /home/cdkbs/abbench/ksweep relaxed 1000 "$k" --txns 2000 \
        > "/home/cdkbs/abbench/ksweep-k$k.log" 2>&1 || echo "FAILED k=$k"
done
echo ALLDONE
