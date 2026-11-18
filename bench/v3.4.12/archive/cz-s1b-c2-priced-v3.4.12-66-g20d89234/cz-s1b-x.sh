#!/bin/bash
# CZ-S1b (raft-marks-2026-11-18.md, the operator's W26): C2 priced further,
# on CZ-S1's spike binary. Two measurements, interleaved, RUNS runs each.
# This is the second pass: cells c1x and c1c2x, measurement A again with the
# inserter held to 150 statements a second, added when the first pass showed
# that at 120 the rolls are short with the prewrite too.
#  A. C1 against C1 with C2 at one load: the `levels` cell with the inserter
#     held to RATE statements a second (`--insert-rate`), relaxed.
#       iso     readers only, the spike off (the bar's denominator)
#       c1h     KDS_CZ_SPIKE=c1
#       c1c2h   KDS_CZ_SPIKE=c1,c2
#  B. the commit path with and without the zero prewrite: 6 readers and 4
#     open-loop one-row UPDATE writers (800/s in all), no walker, no inserter.
#       g-base  group,  the spike off      g-c2   group,  KDS_CZ_SPIKE=c2
#       s-base  strict, the spike off      s-c2   strict, KDS_CZ_SPIKE=c2
#     and the bulk writer at group (the `levels` cell, closed-loop inserter):
#       gb-base group, the spike off       gb-c2  group, KDS_CZ_SPIKE=c2
# The cells rotate their order by one a run. Resumable.
set -u
WT=/home/louis/ckdbs/.claude/worktrees/cz-s1-spike
BASE=/home/louis/bench-runs/cz-s1b
BIN=$1
OUT=$2
PROG=$3
RUNS=${4:-5}
RATE=${5:-150}
mkdir -p "$OUT"
touch "$PROG"
COMMON="--bin $BIN --port 15597 --cores 4 --readers 6 --rate 400 --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --stall-cores --workdir $BASE/work"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
CELLS=(c1x c1c2x)
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s load=$(cut -d' ' -f1 /proc/loadavg)" >> "$PROG"
  done
}
drv() { python3 "$WT/tools/bl_mixed.py" $COMMON "$@"; }
N=${#CELLS[@]}
for r in $(seq 1 "$RUNS"); do
  for k in $(seq 0 $((N - 1))); do
    cell=${CELLS[$(( (k + r - 1) % N ))]}
    D="$OUT/r$r/$cell"
    if ls "$D"/*.json > /dev/null 2>&1; then continue; fi
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg) busy=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | cut -c1-80 | tr '\n' ';')]" >> "$PROG"
    case $cell in
      iso)     env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --durability relaxed --arm isolated --levels reader=transactional --out "$D";;
      c1x)     KDS_CZ_SPIKE=c1    drv --durability relaxed --arm mixed --levels "$LV_ALL" --insert-rate "$RATE" --out "$D";;
      c1c2x)   KDS_CZ_SPIKE=c1,c2 drv --durability relaxed --arm mixed --levels "$LV_ALL" --insert-rate "$RATE" --out "$D";;
      g-base)  env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --durability group  --arm isolated --levels reader=transactional --writers 4 --writer-rate 800 --out "$D";;
      g-c2)    KDS_CZ_SPIKE=c2    drv --durability group  --arm isolated --levels reader=transactional --writers 4 --writer-rate 800 --out "$D";;
      s-base)  env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --durability strict --arm isolated --levels reader=transactional --writers 4 --writer-rate 800 --out "$D";;
      s-c2)    KDS_CZ_SPIKE=c2    drv --durability strict --arm isolated --levels reader=transactional --writers 4 --writer-rate 800 --out "$D";;
      gb-base) env -u KDS_CZ_SPIKE python3 "$WT/tools/bl_mixed.py" $COMMON --durability group  --arm mixed --levels "$LV_ALL" --out "$D";;
      gb-c2)   KDS_CZ_SPIKE=c2    drv --durability group  --arm mixed --levels "$LV_ALL" --out "$D";;
    esac > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) CZS1BX-END" >> "$PROG"
