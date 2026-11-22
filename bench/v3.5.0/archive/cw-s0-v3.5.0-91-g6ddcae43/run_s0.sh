#!/bin/bash
# CW-S0's measurement (workorder-cw-optimistic-page-read.md CW-S0, M1-M5).
# R = the tree at the opening commit (build-release), S = the spike (instrument
# switched by env). Each run writes one JSON; PROGRESS gets a line per run
# start and end, which the 60 s watchdog reads.
set -u
W=/home/louis/bench-runs/cw
TOOL=/home/louis/ckdbs/.claude/worktrees/sync-stale-records/tools/ba_census.py
OUT=$W/out
PROGRESS=$W/progress
mkdir -p "$OUT" "$W/work"
: >> "$PROGRESS"
PORT=17411
COMMON=(--engine kds --durability relaxed --frames 65536 --rows 10000 --warmup 2 --seconds 8
        --runs 1 --workdir "$W/work" --max-load 2.5 --max-wait 120)

cell() {  # cell <label> <bin> <shape> <cores> <server_cpus> <client_cpus> <sessions> [env...]
    local label=$1 bin=$2 shape=$3 cores=$4 scpu=$5 ccpu=$6 sess=$7; shift 7
    local envs=()
    for e in "$@"; do envs+=(--server-env "$e"); done
    local dir=$OUT/$label
    if [ -d "$dir" ] && ls "$dir"/*.json >/dev/null 2>&1; then
        echo "$(date -u +%T) skip $label" >> "$PROGRESS"; return 0
    fi
    echo "$(date -u +%T) start $label" >> "$PROGRESS"
    python3 "$TOOL" "${COMMON[@]}" --bin "$bin" --port $PORT --shape "$shape" --cores "$cores" \
        --server-cpus "$scpu" --client-cpus "$ccpu" --sessions "$sess" --out "$dir" \
        "${envs[@]}" >> "$W/driver.log" 2>&1
    echo "$(date -u +%T) end $label rc=$?" >> "$PROGRESS"
}

R=$W/bin/R
S=$W/bin/S
C1=(1 0 2,3,6,7 2)
C2=(2 0,1 2,3,6,7 4)
C3=(3 0,1,2 3,7,4,5,6 6)   # indicative: clients on the reactors' SMT siblings

for r in 1 2; do
  # M2 and M4 on R, interleaved by run
  for sh in point pkmix insert1; do
    cell "M2-$sh-c1-r$r" $R $sh "${C1[@]}"
    cell "M2-$sh-c2-r$r" $R $sh "${C2[@]}"
    cell "M2-$sh-c3-r$r" $R $sh "${C3[@]}"
  done
  cell "M4-pointN-c2-r$r" $R pointN "${C2[@]}"
  cell "M4-pointN-c3-r$r" $R pointN "${C3[@]}"
  # M5 on S, instrument off: one core armed against unarmed
  for sh in point insert1; do
    cell "M5-$sh-c1u-r$r" $S $sh "${C1[@]}"
    cell "M5-$sh-c1a-r$r" $S $sh "${C1[@]}" KDS_TEST_PAGE_LATCH=1
  done
done
# M1 on S, counts
for sh in point insert1 pkmix; do
  cell "M1-$sh-c2" $S $sh "${C2[@]}" KDS_CW_COUNT=1
done
cell "M1-point-c1a" $S point "${C1[@]}" KDS_CW_COUNT=1 KDS_TEST_PAGE_LATCH=1
# M3's substitute on S, ticks
for sh in point insert1; do
  cell "M3-$sh-c1u" $S $sh "${C1[@]}" KDS_CW_TICKS=1
  cell "M3-$sh-c1a" $S $sh "${C1[@]}" KDS_CW_TICKS=1 KDS_TEST_PAGE_LATCH=1
  cell "M3-$sh-c2" $S $sh "${C2[@]}" KDS_CW_TICKS=1
  cell "M3-$sh-c3" $S $sh "${C3[@]}" KDS_CW_TICKS=1
done
cell "M3-pointN-c2" $S pointN "${C2[@]}" KDS_CW_TICKS=1
cell "M3-pointN-c3" $S pointN "${C3[@]}" KDS_CW_TICKS=1
echo "$(date -u +%T) DONE" >> "$PROGRESS"
