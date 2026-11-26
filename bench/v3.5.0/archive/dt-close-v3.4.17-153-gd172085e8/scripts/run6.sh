#!/bin/bash
# probe: cadence checkpoint off (checkpoint_interval_ms = 0), B and A, cells ivw and au
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
ROOT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8
OUT=$ROOT/probe-ckpt-off
BIN=/home/ukjin/bench-runs/dt/close/bin
W=/home/ukjin/bench-runs/dt/close/work
until [ -e $ROOT/scaling/finished5 ]; do sleep 3; done
mkdir -p $OUT
for arm in B A; do
  echo "$(date -u +%T) probe ckpt-off $arm load $(cat /proc/loadavg) competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)" >> $OUT/driver-all.log
  python3 -I tools/dt_close.py --bin $BIN/kds_server.$arm --arm ${arm}off --cells ivw,au --runs 1 --seconds 20 --warmup 4 \
    --port 32771 --workdir $W --out $OUT --keep --conf-extra "checkpoint_interval_ms = 0" \
    --walk-sql "UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000" >> $OUT/driver-$arm.log 2>&1
done
touch $OUT/finished6
