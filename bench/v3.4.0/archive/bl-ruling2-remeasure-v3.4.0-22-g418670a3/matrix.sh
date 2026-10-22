#!/bin/bash
# BL ruling 2 re-measure: C1 (mixed, relaxed, cores=4) x {isolated, segregated-no0, spread, batch0} x 3 runs, interleaved.
WT=/home/cdkbs/ckdbs/.claude/worktrees/bl-ruling2-remeasure
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
BIN=$1
OUT=$2
PROG=$SP/matrix.progress
mkdir -p "$OUT"
: > "$PROG"
COMMON="--bin $BIN --port 15581 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core 0 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --runs 1"
for r in 1 2 3; do
  for cell in "isolated segregated-no0" "mixed segregated-no0" "mixed spread" "mixed batch0"; do
    set -- $cell
    echo "$(date -u +%H:%M:%S) start r$r $1 $2" >> "$PROG"
    python3 "$WT/tools/bl_mixed.py" $COMMON --arm "$1" --placement "$2" --out "$OUT/r$r" > "$OUT/console-r$r-$1-$2.log" 2>&1; rc=$?
    echo "$(date -u +%H:%M:%S) done r$r $1 $2 rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%H:%M:%S) MATRIX-END" >> "$PROG"
