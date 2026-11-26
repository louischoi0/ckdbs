#!/bin/bash
# Resume after a hold: another worktree's build and ctest overlapped B r1 (its cells are in contaminated/).
# Waits for a quiet host, then runs B r1, A r1, and DT-R4.
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0
BIN=/home/ukjin/bench-runs/dt/close/bin
W=/home/ukjin/bench-runs/dt/close/work
WSQL="UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000"
comp() { { pgrep -x cc1plus; pgrep -x ctest; pgrep -x cmake; pgrep -x make; } | wc -l; }
bash $OUT/scripts/quiet.sh >> $OUT/driver-all.log
invoke() { # arm run cells
  echo "$(date -u +%H:%M:%S) invoke arm $1 run $2 cells $3 load $(cat /proc/loadavg) competing(exact names): $(comp)" >> $OUT/driver-all.log
  python3 -I tools/dt_close.py --bin $BIN/kds_server.$1 --arm $1 --cells $3 --runs 1 --run-base $2 \
     --port 32771 --workdir $W --out $OUT --keep --walk-sql "$WSQL" >> $OUT/driver-$1-r$2.log 2>&1
  echo "$(date -u +%H:%M:%S) done arm $1 run $2 rc=$? load $(cat /proc/loadavg) competing(exact names): $(comp)" >> $OUT/driver-all.log
}
invoke B 1 ivw,ivu,aw,au
invoke A 1 ivw,ivu,aw,au
echo "$(date -u +%H:%M:%S) cells finished" >> $OUT/driver-all.log
touch $OUT/finished
mkdir -p /home/ukjin/bench-runs/dt/close/w4r
rm -rf /home/ukjin/bench-runs/dt/close/w4r/*
bash $OUT/scripts/quiet.sh >> $OUT/driver-all.log
echo "$(date -u +%T) cell5 start load $(cat /proc/loadavg) competing(exact names): $(comp)" >> $OUT/driver-all.log
python3 -I tools/dr_close.py --part ab --bin-a $BIN/kds_server.A --bin-b $BIN/kds_server.C \
  --workdir /home/ukjin/bench-runs/dt/close/w4r --out $OUT/dr4-ab.json --rounds 15 --port 32781 --heartbeat $OUT/dr4.beat > $OUT/driver-dr4.log 2>&1
echo "$(date -u +%T) cell5 done rc=$? load $(cat /proc/loadavg) competing(exact names): $(comp)" >> $OUT/driver-all.log
touch $OUT/finished4
