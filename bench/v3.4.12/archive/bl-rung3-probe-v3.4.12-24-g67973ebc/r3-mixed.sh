#!/bin/bash
# BL rung-3 probe (BL §5.2): cores=4, the balancer placing, readers transactional.
# Arms, interleaved, 5 runs each:
#   isolated  readers only
#   levels    readers + analytic walker + background inserter (the BL close's cell)
#   probe     levels + one walker at normal (shares the interactive cores)
#   nwonly    readers + one walker at normal, no batch session (no partition)
set -u
WT=/home/louis/ckdbs/.claude/worktrees/bl-rung3-probe
OUT=$1
PROG=$2
RUNS=${3:-5}
mkdir -p "$OUT"
: > "$PROG"
COMMON="--bin /home/louis/bench-runs/bl-rung3/bin/kds_server.48857a5a --port 15593 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --workdir /home/louis/bench-runs/bl-rung3/work"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s" >> "$PROG"
  done
}
for r in $(seq 1 "$RUNS"); do
  for cell in isolated levels probe nwonly; do
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
    case $cell in
      isolated) python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --out "$OUT/r$r";;
      levels)   python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$OUT/r$r";;
      probe)    python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --normal-walkers 1 --out "$OUT/r$r";;
      nwonly)   python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --normal-walkers 1 --out "$OUT/r$r";;
    esac > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) MIXED-END" >> "$PROG"
