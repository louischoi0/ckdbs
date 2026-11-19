#!/bin/bash
W=/home/louis/bench-runs/db-s0
for c in 1 4; do
  echo "pre-run c=$c: load $(cut -d' ' -f1-3 /proc/loadavg) $(pgrep -a -f 'cc1plus|cmake --build|ctest' | head -3)" >> $W/hostload.log
  python3 /home/louis/ckdbs/.claude/worktrees/db-torn-page-healing/tools/db_s0_premise.py --bin $W/kds_server --workdir $W --port 1741$c --cores $c --out $W/out-c$c.jsonl >> $W/driver-c$c.log 2>&1
done
echo FINISHED > $W/finished
