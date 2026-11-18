#!/bin/bash
# BL rung-3 probe, forced sharing: the balancer off (KDS_BL_NO_BALANCE, the
# measurement build from the BL close's nobal.patch), every session on core 1.
#   same-iso  readers only (transactional)
#   same-nw   readers + one walker at normal on the same core
# This is the most rung 3 could reorder: what one `normal` walk costs a
# `transactional` reader whose core it shares.
set -u
WT=/home/louis/ckdbs/.claude/worktrees/bl-rung3-probe
OUT=$1
PROG=$2
RUNS=${3:-5}
mkdir -p "$OUT"
: > "$PROG"
export KDS_BL_NO_BALANCE=1
COMMON="--bin /home/louis/bench-runs/bl-rung3/bin/kds_server.48857a5a-nobal --port 15593 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement same --core 1 --runs 1 --meta-cores --workdir /home/louis/bench-runs/bl-rung3/work"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s" >> "$PROG"
  done
}
for r in $(seq 1 "$RUNS"); do
  for cell in same-iso same-nw; do
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
    case $cell in
      same-iso) python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --out "$OUT/r$r";;
      same-nw)  python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --normal-walkers 1 --out "$OUT/r$r";;
    esac > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) MIXED-END" >> "$PROG"
