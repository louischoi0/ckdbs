#!/bin/bash
# CZ-S1 (workorder-cz-checkpoint-roll-wait.md §5): the ceiling, by a spike that
# is not landed. The rung-3 probe's `levels` cell (CZ-S0's), one binary, the
# arm chosen by KDS_CZ_SPIKE. Arms, interleaved, RUNS runs each:
#   isolated  readers only, the spike off (the bar's denominator)
#   levels    readers + analytic walker + background inserter, the spike off
#   c1        levels with KDS_CZ_SPIKE=c1    (the checkpoint's latch sites try and park)
#   c2        levels with KDS_CZ_SPIKE=c2    (no zero prewrite where fallocate reserved)
#   c1c2      levels with KDS_CZ_SPIKE=c1,c2
# `isolated` runs first in every run; the four loaded arms rotate their order
# by one a run. Resumable: a cell whose JSON exists is skipped.
set -u
WT=/home/louis/ckdbs/.claude/worktrees/cz-checkpoint-roll-wait
BASE=/home/louis/bench-runs/cz-s1
BIN=$1
OUT=$2
PROG=$3
RUNS=${4:-5}
mkdir -p "$OUT"
touch "$PROG"
COMMON="--bin $BIN --port 15597 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --stall-cores --workdir $BASE/work"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
LOADED=(levels c1 c2 c1c2)
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s load=$(cut -d' ' -f1 /proc/loadavg)" >> "$PROG"
  done
}
for r in $(seq 1 "$RUNS"); do
  order=(isolated)
  for k in 0 1 2 3; do order+=("${LOADED[$(( (k + r - 1) % 4 ))]}"); done
  for cell in "${order[@]}"; do
    D="$OUT/r$r/$cell"
    if ls "$D"/*.json > /dev/null 2>&1; then continue; fi
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg) busy=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | cut -c1-80 | tr '\n' ';')]" >> "$PROG"
    case $cell in
      isolated) env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --out "$D";;
      levels)   env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$D";;
      c1)       KDS_CZ_SPIKE=c1 python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$D";;
      c2)       KDS_CZ_SPIKE=c2 python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$D";;
      c1c2)     KDS_CZ_SPIKE=c1,c2 python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$D";;
    esac > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) CZS1-END" >> "$PROG"
