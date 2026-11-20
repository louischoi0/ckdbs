#!/bin/bash
# wait for gate.sh to settle, then run the driver detached-style (this script itself is run under nohup)
cd /home/louis/bench-runs/dl-s0
until grep -q '^settled' gate.status 2>/dev/null; do sleep 3; done
date -u +%F\ %T > driver.start
python3 dl_s0.py --bin /home/louis/bench-runs/dl-s0/kds_spike \
  --tools /home/louis/ckdbs/.claude/worktrees/dl-statistics-queue-no-statement/tools \
  --wd /home/louis/bench-runs/dl-s0/run --out /home/louis/bench-runs/dl-s0/raw.jsonl \
  --status /home/louis/bench-runs/dl-s0/run.status --port 27700 > driver.log 2>&1
date -u +%F\ %T > driver.end
