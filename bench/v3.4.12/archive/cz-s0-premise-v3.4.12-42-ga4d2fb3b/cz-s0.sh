#!/bin/bash
# CZ-S0 (workorder-cz-checkpoint-roll-wait.md §5): the premise cells M1-M4.
# The rung-3 probe's cell (r3-mixed.sh): cores=4, relaxed, the balancer
# placing, readers transactional. Arms, interleaved, RUNS runs each:
#   isolated  readers only, the default cadence (5 s)
#   levels    readers + analytic walker + background inserter (M1's default arm, M2)
#   nockpt    levels with checkpoint_interval_ms = 0 (M1's cadence-off arm, V0's number; M3)
#   iso0      isolated with checkpoint_interval_ms = 0 (a control the order does not name)
#   half      levels with the inserter's batch halved, 512 rows (M4)
# Resumable: a cell whose JSON exists is skipped.
set -u
WT=/home/louis/ckdbs/.claude/worktrees/cz-checkpoint-roll-wait
BASE=/home/louis/bench-runs/cz-s0
OUT=$1
PROG=$2
RUNS=${3:-5}
mkdir -p "$OUT"
touch "$PROG"
COMMON="--bin $BASE/bin/kds_server.a4d2fb3b --port 15597 --cores 4 --readers 6 --rate 400 --durability relaxed --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --workdir $BASE/work"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
NOCK="checkpoint_interval_ms = 0"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s load=$(cut -d' ' -f1 /proc/loadavg)" >> "$PROG"
  done
}
for r in $(seq 1 "$RUNS"); do
  for cell in isolated levels nockpt iso0 half; do
    D="$OUT/r$r/$cell"
    if ls "$D"/*.json > /dev/null 2>&1; then continue; fi
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg) busy=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | cut -c1-80 | tr '\n' ';')]" >> "$PROG"
    case $cell in
      isolated) python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --out "$D";;
      levels)   python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --out "$D";;
      nockpt)   python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --extra-conf "$NOCK" --out "$D";;
      iso0)     python3 "$WT/tools/bl_mixed.py" $COMMON --arm isolated --levels reader=transactional --extra-conf "$NOCK" --out "$D";;
      half)     python3 "$WT/tools/bl_mixed.py" $COMMON --arm mixed --levels "$LV_ALL" --batch 512 --out "$D";;
    esac > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) CZS0-END" >> "$PROG"
