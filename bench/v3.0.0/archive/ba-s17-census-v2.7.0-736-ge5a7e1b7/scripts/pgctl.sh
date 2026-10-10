#!/bin/bash
# Device-drift control for the PostgreSQL rows read from BA-S4: a few of S4's `on` cells re-run today, same driver and flags.
# This is NOT the PostgreSQL half of the matrix; it prices how much the synced numbers moved with the device between the two days.
W=/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks
R=/home/cdkbs/bench-runs/ba-s17
OUT=$R/pgctl
WORK=$R/work
mkdir -p $OUT
until test -f /home/cdkbs/bench-runs/ba-close/post.done; do sleep 20; done
COMMON="--runs 3 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 --max-load 6 --max-wait 300 --workdir $WORK"
for cell in "insert1 0 1" "insert1 0 16" "insert1 0,2 16" "trade 0,2 16" "insertN 0,2 16" "recent 0,2 16" "insert1 0,2 1"; do
  set -- $cell
  while pgrep -x ctest > /dev/null || pgrep -x cc1plus > /dev/null || pgrep -x kds_tests > /dev/null; do sleep 15; done
  python3 $W/tools/ba_census.py --engine pg --pg-port 15434 --shape $1 --durability on --sessions $3 --server-cpus $2 --out $OUT/pg-cpus$(echo $2 | tr , -) $COMMON >> $R/pgctl.log 2>&1
done
date -u > $R/pgctl-done.flag
