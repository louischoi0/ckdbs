#!/bin/bash
# DT close: cells 1-3, interleaved by round (A,B / B,A), then cell 4.
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8
BIN=/home/ukjin/bench-runs/dt/close/bin
W=/home/ukjin/bench-runs/dt/close/work
mkdir -p $OUT
WSQL="UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000"
invoke() { # arm run
  echo "$(date -u +%H:%M:%S) invoke arm $1 run $2 load $(cat /proc/loadavg) competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)" >> $OUT/driver-all.log
  python3 -I tools/dt_close.py --bin $BIN/kds_server.$1 --arm $1 --cells ivw,ivr,i,ivu,aw,au --runs 1 --run-base $2 \
     --port 32771 --workdir $W --out $OUT --keep --walk-sql "$WSQL" >> $OUT/driver-$1-r$2.log 2>&1
  echo "$(date -u +%H:%M:%S) done arm $1 run $2 rc=$? load $(cat /proc/loadavg) competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)" >> $OUT/driver-all.log
}
invoke A 0; invoke B 0; invoke B 1; invoke A 1
echo "$(date -u +%H:%M:%S) cells 1-3 finished" >> $OUT/driver-all.log
touch $OUT/finished
