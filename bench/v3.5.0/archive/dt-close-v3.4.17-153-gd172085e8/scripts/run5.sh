#!/bin/bash
# writing walk alone, rows written per walk varied (1 run each, interleaved A,B per size)
cd /home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield || exit 1
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8/scaling
BIN=/home/ukjin/bench-runs/dt/close/bin
W=/home/ukjin/bench-runs/dt/close/work
until [ -e $OUT/../finished4 ]; do sleep 3; done
mkdir -p $OUT
for N in 5000 15000; do
  for arm in A B; do
    echo "$(date -u +%T) scaling $arm N=$N load $(cat /proc/loadavg) competing: $(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)" >> $OUT/driver-all.log
    python3 -I tools/dt_close.py --bin $BIN/kds_server.$arm --arm ${arm}n$N --cells au --runs 1 --seconds 20 --warmup 2 \
      --port 32771 --workdir $W --out $OUT --keep --walk-sql "UPDATE dt_big SET c0 = c0 + 1 WHERE c0 >= 1 AND c0 <= $N" >> $OUT/driver-$arm-$N.log 2>&1
  done
done
touch $OUT/finished5
