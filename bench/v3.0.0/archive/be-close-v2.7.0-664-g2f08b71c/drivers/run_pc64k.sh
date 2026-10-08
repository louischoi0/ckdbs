#!/bin/bash
R=/home/cdkbs/bench-runs/be-close
cd /home/cdkbs/ckdbs/.claude/worktrees/pool-budget-required
for ARM in B A; do
  echo "== $ARM start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/pc64k.log
  python3 -I $R/driver/pool_past_cap.py $ARM 65536 --big2 7077888 --out $R/pastcap/cap65536-$ARM.json >> $R/pc64k.log 2>&1
  echo "== $ARM end $(date -u +%H:%M:%S)" >> $R/pc64k.log
done
echo done >> $R/pc64k.log
