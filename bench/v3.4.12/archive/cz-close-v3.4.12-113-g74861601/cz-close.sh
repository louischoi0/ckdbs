#!/bin/bash
# CZ-S4 (workorder-cz-checkpoint-roll-wait.md §5): the close. A is origin/main
# at the commit CZ-S3 branched from (no CZ code), B is CZ-S3. Interleaved:
#   V1 (CZ-Q1 (a), BL's bar (v), CZ-Q11 (a)): the rung-3 probe's `levels` cell
#      at relaxed - isolated-B, levels-A, levels-B - RUNS runs, order rotated
#   C3 (CZ-Q2 (a)): BQ's bulk writer alone, A and B, RUNS runs
#   G  (CZ-Q5 (a)): `levels` and isolated at group, A and B, GRUNS runs
# Then the cores=1 overhead (cu_close_overhead.py, A, B, A2 = a copy of A).
# Resumable: a cell whose JSON exists is skipped.
set -u
WT=/home/louis/ckdbs/.claude/worktrees/cz-checkpoint-roll-wait
BASE=/home/louis/bench-runs/cz-close
A=$1; B=$2; OUT=$3; PROG=$4; RUNS=${5:-5}; GRUNS=${6:-3}
mkdir -p "$OUT"; touch "$PROG"
COMMON="--port 15597 --cores 4 --readers 6 --rate 400 --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --workdir $BASE/work"
LV_ALL="reader=transactional,walker=analytic,inserter=background"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s load=$(cut -d' ' -f1 /proc/loadavg)" >> "$PROG"
  done
}
cell() {  # name dir args...
  local name=$1 D=$2; shift 2
  if ls "$D"/*.json > /dev/null 2>&1; then return; fi
  quiet
  echo "$(date -u +%T) start $name load=$(cut -d' ' -f1-3 /proc/loadavg) busy=[$(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | cut -c1-80 | tr '\n' ';')]" >> "$PROG"
  python3 "$WT/tools/bl_mixed.py" "$@" --out "$D" > "$OUT/console-$name.log" 2>&1
  echo "$(date -u +%T) done $name rc=$?" >> "$PROG"
}
V1=(levels-A levels-B)
for r in $(seq 1 "$RUNS"); do
  cell "r$r-isolated-B" "$OUT/v1/r$r/isolated-B" $COMMON --bin "$B" --durability relaxed --arm isolated --levels reader=transactional
  for k in 0 1; do
    a=${V1[$(( (k + r - 1) % 2 ))]}; bin=$A; [ "$a" = levels-B ] && bin=$B
    cell "r$r-$a" "$OUT/v1/r$r/$a" $COMMON --bin "$bin" --durability relaxed --arm mixed --levels "$LV_ALL"
  done
  for k in 0 1; do
    a=$([ $(( (k + r) % 2 )) -eq 0 ] && echo A || echo B); bin=$A; [ "$a" = B ] && bin=$B
    cell "r$r-bulk-$a" "$OUT/c3/r$r/bulk-$a" $COMMON --bin "$bin" --durability relaxed --arm insert --readers 0 --levels inserter=background
  done
done
for r in $(seq 1 "$GRUNS"); do
  cell "r$r-g-isolated-B" "$OUT/g/r$r/isolated-B" $COMMON --bin "$B" --durability group --arm isolated --levels reader=transactional
  for k in 0 1; do
    a=$([ $(( (k + r) % 2 )) -eq 0 ] && echo A || echo B); bin=$A; [ "$a" = B ] && bin=$B
    cell "r$r-g-levels-$a" "$OUT/g/r$r/levels-$a" $COMMON --bin "$bin" --durability group --arm mixed --levels "$LV_ALL"
  done
done
if [ ! -f "$OUT/overhead/q1.json" ]; then
  mkdir -p "$OUT/overhead"; quiet
  echo "$(date -u +%T) start overhead" >> "$PROG"
  python3 "$WT/tools/cu_close_overhead.py" --arm A="$A" --arm B="$B" --arm A2="${A}2" \
    --workdir "$BASE/ovh" --out "$OUT/overhead/q1.json" --progress "$OUT/overhead/q1.progress" --runs 5 \
    > "$OUT/console-overhead.log" 2>&1
  echo "$(date -u +%T) done overhead rc=$?" >> "$PROG"
fi
echo "$(date -u +%T) CZCLOSE-END" >> "$PROG"
