#!/bin/bash
R=/home/cdkbs/bench-runs/be-close
cd /home/cdkbs/ckdbs/.claude/worktrees/pool-budget-required
for ARM in B A; do
  echo "== $ARM start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/sr.log
  python3 -I $R/driver/scanres.py $ARM 16384 3538944 --out $R/pastcap/scanres-16384-$ARM.json >> $R/sr.log 2>&1
  echo "== $ARM end $(date -u +%H:%M:%S)" >> $R/sr.log
done
echo done >> $R/sr.log
