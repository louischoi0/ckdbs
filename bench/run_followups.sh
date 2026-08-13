#!/usr/bin/env bash
# Two follow-ups: the ordinary-statement arms repeated at the largest row set
# (where one arm disagreed with its own unasserted control), and a server-CPU
# cell sized so a 10 ms scheduler tick is a small error rather than the whole
# signal.
set -euo pipefail
W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2

for rep in 1 2; do
    echo "=== ordinary repeat $rep ==="
    "$W/bench/run_abort_sweep.sh" "/home/cdkbs/abbench/ord$rep" relaxed 10000 16 \
        --txns 100 --ordinary-ops 4000 --cpu-rounds 0 \
        > "/home/cdkbs/abbench/ord$rep.log" 2>&1 || echo "FAILED ord$rep"
done

echo "=== server CPU cell, K=16 ==="
"$W/bench/run_abort_sweep.sh" /home/cdkbs/abbench/cpucell relaxed 1000 16 \
    --txns 200 --cpu-rounds 3 --cpu-txns 6000 \
    > /home/cdkbs/abbench/cpucell.log 2>&1 || echo "FAILED cpucell"
echo ALLDONE
