#!/bin/bash
# DG-S0 (instructions/v3.2.0/workorder-dg-wal-segment-body.md §5): a WAL
# segment's body priced, on a measurement-only spike binary (KDS_DG_SPIKE,
# dg-s0-spike.patch beside this file). Cells, RUNS runs each, rotated by one a
# run; resumable (a cell with a JSON is skipped).
#
# The bulk writer (the `levels` cell: 6 readers at 400/s, the walker, the
# closed-loop 1,024-row inserter), at `group` (bg-*) and `relaxed` (br-*):
#   base   the fill as it is            nofill  P1: no fill where fallocated
#   pw     M2: 32 bodies filled at Open, warm    pc  M2: the same, cold
#   pace   M3/P2: the fill paced (1 MiB, sync_file_range, 8 ms sleep), asked
#          for at the segment's start
# M1, the commit under a log that rolls: 6 readers at 400/s, 4 one-row UPDATE
# writers at 800/s in all, the inserter held to 400 statements/s of 128 rows
# (a commit of a block or more), at `group` (mg-*) and `strict` (ms-*):
#   base, nofill, pw (12 bodies, warm)
# and 16 writers at 4,000/s, so that `group` groups (gg-*): base, nofill.
# M4 is on every arm: each body's reserve, write and fsync are stamped in
# <cell>/fill.log, with the ask at the half mark and each removed segment.
set -u
BASE=/home/louis/bench-runs/dg-s0
WT=/home/louis/ckdbs/.claude/worktrees/dg-wal-segment-body
BIN=$1
OUT=$2
PROG=$3
RUNS=${4:-3}
mkdir -p "$OUT" "$BASE/work"
touch "$PROG"
COMMON="--bin $BIN --port 15611 --cores 4 --readers 6 --rate 400 --seconds 10 --warmup 2 --frames 65536 --log-level info --ctl-core -1 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --placement kernel --runs 1 --meta-cores --workdir $BASE/work"
LV_BULK="reader=transactional,walker=analytic,inserter=background"
LV_M1="reader=transactional,inserter=background"
BULK="--arm mixed --levels $LV_BULK"
M1="--arm insert --levels $LV_M1 --writers 4 --writer-rate 800 --batch 128 --insert-rate 400"
GG="--arm insert --levels $LV_M1 --writers 16 --writer-rate 4000 --batch 128 --insert-rate 400"
CELLS=(bg-base bg-nofill bg-pw bg-pc bg-pace br-base br-nofill br-pw br-pc br-pace
       mg-base mg-nofill mg-pw ms-base ms-nofill ms-pw gg-base gg-nofill)
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        pgrep -x ld > /dev/null || [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
    echo "$(date -u +%T) gate waiting ${waited}s load=$(cut -d' ' -f1 /proc/loadavg)" >> "$PROG"
  done
}
N=${#CELLS[@]}
for r in $(seq 1 "$RUNS"); do
  for k in $(seq 0 $((N - 1))); do
    cell=${CELLS[$(( (k + r - 1) % N ))]}
    D="$OUT/r$r/$cell"
    if ls "$D"/*.json > /dev/null 2>&1; then continue; fi
    rm -rf "$D"; mkdir -p "$D"
    quiet
    echo "$(date -u +%T) start r$r $cell load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
    case ${cell#*-} in
      base)   SP="" ;;
      nofill) SP="nofill" ;;
      pw)     case $cell in b*) SP="pool=32" ;; *) SP="pool=12" ;; esac ;;
      pc)     SP="pool=32,cold" ;;
      pace)   SP="pace=8000" ;;
    esac
    case ${cell%%-*} in
      bg) SHAPE="$BULK"; DUR=group ;;
      br) SHAPE="$BULK"; DUR=relaxed ;;
      mg) SHAPE="$M1"; DUR=group ;;
      ms) SHAPE="$M1"; DUR=strict ;;
      gg) SHAPE="$GG"; DUR=group ;;
    esac
    KDS_DG_SPIKE="$SP" KDS_DG_FILL_LOG="$D/fill.log" \
      python3 "$WT/tools/bl_mixed.py" $COMMON --durability $DUR $SHAPE --out "$D" \
      > "$OUT/console-r$r-$cell.log" 2>&1
    rc=$?
    echo "$(date -u +%T) done r$r $cell spike=[$SP] rc=$rc" >> "$PROG"
  done
done
echo "$(date -u +%T) ALL DONE" >> "$PROG"
