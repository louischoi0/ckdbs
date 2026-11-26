#!/bin/bash
# Rerun of the cells another worktree's builds/ctest overlapped (see driver-all.log, contaminated/).
# A sampler notes every second whether a real competitor (exact process names cc1plus, ctest, cmake, make) exists;
# a cell runs only after 45 s without one and is moved to contaminated/ and retried (max 4) if one appeared during it.
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0
BIN=/home/ukjin/bench-runs/dt/close/bin
W=/home/ukjin/bench-runs/dt/close/work
WSQL="UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= 50000"
LAST=$OUT/last_comp
echo 0 > $LAST
sampler() {
  while [ ! -e $OUT/finished4 ]; do
    if pgrep -x cc1plus >/dev/null || pgrep -x ctest >/dev/null || pgrep -x cmake >/dev/null || pgrep -x make >/dev/null; then
      date +%s > $LAST
      echo "$(date -u +%T) competitor: $(pgrep -x cc1plus | wc -l) cc1plus, $(pgrep -x ctest | wc -l) ctest, $(pgrep -x cmake | wc -l) cmake" >> $OUT/sampler.log
    fi
    sleep 1
  done
}
sampler &
SP=$!
gate() { # wait until the last competitor is older than $1 seconds
  local w=0
  while [ $(( $(date +%s) - $(cat $LAST) )) -lt $1 ]; do sleep 3; w=$((w+3)); done
  echo "$(date -u +%T) gate open after waiting $w s" >> $OUT/driver-all.log
}
cellrun() { # arm run cell
  for try in 1 2 3 4; do
    gate 45
    t0=$(date +%s)
    echo "$(date -u +%H:%M:%S) invoke arm $1 run $2 cell $3 try $try load $(cat /proc/loadavg)" >> $OUT/driver-all.log
    python3 -I tools/dt_close.py --bin $BIN/kds_server.$1 --arm $1 --cells $3 --runs 1 --run-base $2 \
      --port 32771 --workdir $W --out $OUT --keep --walk-sql "$WSQL" >> $OUT/driver-$1-r$2.log 2>&1
    if [ $(cat $LAST) -ge $t0 ]; then
      echo "$(date -u +%H:%M:%S) cell $1 r$2 $3 try $try overlapped a competitor; discarded" >> $OUT/driver-all.log
      mkdir -p $OUT/contaminated/rerun-try$try
      mv $OUT/dt-$3-$1-r$2.* $OUT/contaminated/rerun-try$try/ 2>/dev/null
    else
      echo "$(date -u +%H:%M:%S) cell $1 r$2 $3 try $try clean (no competitor sampled during it)" >> $OUT/driver-all.log
      return 0
    fi
  done
  echo "$(date -u +%H:%M:%S) cell $1 r$2 $3 NOT CLEAN after 4 tries" >> $OUT/driver-all.log
}
cellrun B 0 ivw
cellrun C 0 ivw
cellrun A 1 ivw
cellrun B 0 ivu
cellrun B 0 aw
cellrun B 0 au
echo "$(date -u +%H:%M:%S) rerun cells finished" >> $OUT/driver-all.log
touch $OUT/finished
mkdir -p /home/ukjin/bench-runs/dt/close/w4r
for try in 1 2 3 4; do
  rm -rf /home/ukjin/bench-runs/dt/close/w4r/*
  gate 45
  t0=$(date +%s)
  echo "$(date -u +%T) cell5 start try $try load $(cat /proc/loadavg)" >> $OUT/driver-all.log
  python3 -I tools/dr_close.py --part ab --bin-a $BIN/kds_server.A --bin-b $BIN/kds_server.C \
    --workdir /home/ukjin/bench-runs/dt/close/w4r --out $OUT/dr4-ab.json --rounds 15 --port 32781 --heartbeat $OUT/dr4.beat > $OUT/driver-dr4.log 2>&1
  echo "$(date -u +%T) cell5 done rc=$? load $(cat /proc/loadavg)" >> $OUT/driver-all.log
  if [ $(cat $LAST) -ge $t0 ]; then
    echo "$(date -u +%T) cell5 try $try overlapped a competitor" >> $OUT/driver-all.log
    mkdir -p $OUT/contaminated/dr4-try$try
    cp $OUT/dr4-ab.json $OUT/driver-dr4.log $OUT/contaminated/dr4-try$try/ 2>/dev/null
  else
    echo "$(date -u +%T) cell5 try $try clean" >> $OUT/driver-all.log
    break
  fi
done
touch $OUT/finished4
