#!/bin/bash
# CW-S0: the order's "largest clean cell" on this host's current topology
# (siblings (0,4) (1,5) (2,6) (3,7)): three reactors on 0, 1, 2 and every
# client on the fourth physical core (3, 7). Run after run_m3.sh.
set -u
W=/home/louis/bench-runs/cw
TOOL=/home/louis/ckdbs/.claude/worktrees/sync-stale-records/tools/ba_census.py
PROGRESS=$W/progress
cell() {  # cell <label> <bin> <shape> [env...]
    local label=$1 bin=$2 shape=$3; shift 3
    local envs=()
    for e in "$@"; do envs+=(--server-env "$e"); done
    [ -d "$W/out/$label" ] && ls "$W/out/$label"/*.json >/dev/null 2>&1 && return 0
    echo "$(date -u +%T) start $label" >> "$PROGRESS"
    python3 "$TOOL" --engine kds --durability relaxed --frames 65536 --rows 10000 --warmup 2 \
        --seconds 8 --runs 1 --workdir "$W/work" --max-load 2.5 --max-wait 180 --bin "$bin" \
        --port 17411 --shape "$shape" --cores 3 --server-cpus 0,1,2 --client-cpus 3,7 \
        --sessions 6 --out "$W/out/$label" "${envs[@]}" >> "$W/driver.log" 2>&1
    echo "$(date -u +%T) end $label rc=$?" >> "$PROGRESS"
}
for r in 1 2; do
  cell "C3c-point-r$r" $W/bin/R point
  cell "C3c-pointN-r$r" $W/bin/R pointN
done
cell "C3c-M3-point" $W/bin/S point KDS_CW_TICKS=1
cell "C3c-M3-pointN" $W/bin/S pointN KDS_CW_TICKS=1
cell "C3c-M3-insert1" $W/bin/S insert1 KDS_CW_TICKS=1
echo "$(date -u +%T) C3c DONE" >> "$PROGRESS"
