#!/bin/bash
R=/home/cdkbs/bench-runs/be-close
mkdir -p $R/pastcap
cd /home/cdkbs/ckdbs/.claude/worktrees/pool-budget-required
for ARM in B A; do
  echo "== $ARM start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/pc16k.log
  python3 -I $R/driver/pool_past_cap.py $ARM 16384 --big2 1769472 --big4 3538944 --hot 1024 --out $R/pastcap/cap16384-$ARM.json >> $R/pc16k.log 2>&1
  echo "== $ARM end $(date -u +%H:%M:%S)" >> $R/pc16k.log
done
echo done >> $R/pc16k.log
