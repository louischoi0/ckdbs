#!/usr/bin/env bash
# Row-set sweep at fixed K, carrying the ordinary-statement regression arms.
# CPU sampling is off here: at these window sizes /proc's jiffy counter cannot
# resolve the signal, and it gets its own properly-sized cell instead.
set -euo pipefail
W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2

run() {  # rows ops
    "$W/bench/run_abort_sweep.sh" /home/cdkbs/abbench/rowsweep relaxed "$1" 16 \
        --txns 2000 --ordinary-ops "$2" --cpu-rounds 0 \
        > "/home/cdkbs/abbench/rowsweep-r$1.log" 2>&1 || echo "FAILED rows=$1"
}

echo "=== rows=200 ==="   ; run 200 200
echo "=== rows=1000 ==="  ; run 1000 1000
echo "=== rows=10000 ===" ; run 10000 2000
echo "=== K=32 repeat ==="
"$W/bench/run_abort_sweep.sh" /home/cdkbs/abbench/k32rep relaxed 1000 32 \
    --txns 2000 --cpu-rounds 0 > /home/cdkbs/abbench/k32rep.log 2>&1 || echo "FAILED k32rep"
echo ALLDONE
