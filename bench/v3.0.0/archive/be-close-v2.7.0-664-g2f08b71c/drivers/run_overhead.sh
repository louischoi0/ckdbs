#!/bin/bash
R=/home/cdkbs/bench-runs/be-close
cd /home/cdkbs/ckdbs/.claude/worktrees/pool-budget-required
taskset -c 4 python3 -I $R/driver/be_overhead_ab.py --rows 200,1000,10000 --runs 12 --pin-server 2 \
  --bin-a $R/bin/kds_server-A --bin-b $R/bin/kds_server-B --workdir $R/w --out $R/overhead \
  --port-a 15611 --port-b 15610 --max-load 2.5 > $R/overhead.log 2>&1
echo done >> $R/overhead.log
