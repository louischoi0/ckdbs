#!/bin/bash
# BL-S5 mixed cell on B (f4bad273) at cores=4, the balancer placing:
# isolated readers (transactional), mixed with levels, mixed with none; 3 runs each, interleaved.
set -u
WT=/home/cdkbs/ckdbs/.claude/worktrees/bl-s4-partition
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
OUT=$1
PROG=$SP/s5-mixed.progress
mkdir -p "$OUT"
: > "$PROG"
COMMON="--bin /home/cdkbs/bench-runs/bl/bin/kds_server.f4bad273 --port 15592 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
  done
  [ $waited -gt 0 ] && echo "$(date -u +%T) gate waited ${waited}s" >> "$PROG"
}
for r in 1 2 3; do
  for cell in isolated levels none; do
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
    case $cell in
      isolated) python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --out "$OUT/r$r" > "$OUT/console-r$r-$cell.log" 2>&1;;
      levels)   python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$OUT/r$r" > "$OUT/console-r$r-$cell.log" 2>&1;;
      none)     python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --out "$OUT/r$r" > "$OUT/console-r$r-$cell.log" 2>&1;;
    esac
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) MIXED-END" >> "$PROG"
