#!/bin/bash
# second pass: wait for a quiet host (no build/suite, no other kds_ server, load1 < 4 for 6 checks in a row,
# at most until 05:58 UTC), then rerun the cores-8 cells whose first pass was beside another session's driver.
cd /home/louis/bench-runs/dl-s0
q=0; t0=$(date +%s)
while [ $q -lt 6 ]; do
  busy=$(pgrep -f "cc1plus|cmake --build|ctest" | grep -v -w $$ | wc -l)
  other=$(ps -eo args | grep -E '^[^ ]*kds_' | grep -v 'dl-s0/kds_spike' | grep -v build-sandbox | wc -l)
  la=$(cut -d' ' -f1 /proc/loadavg | cut -d. -f1)
  if [ "$busy" = 0 ] && [ "$other" = 0 ] && [ "$la" -lt 4 ]; then q=$((q+1)); else q=0; fi
  echo "$(date -u +%T) held $(( $(date +%s)-t0 ))s busy=$busy other=$other load=$la q=$q" > gate3.status
  if [ "$(date -u +%H%M)" -ge 0642 ]; then echo "gave up $(date -u +%T)" > gate3.status; exit 1; fi
  sleep 10
done
echo "settled $(date -u +%T) held $(( $(date +%s)-t0 ))s" > gate3.status
rm -f driver2.end
date -u +%F\ %T > driver2.start
python3 dl_s0b.py --bin /home/louis/bench-runs/dl-s0/kds_spike \
  --tools /home/louis/ckdbs/.claude/worktrees/dl-statistics-queue-no-statement/tools \
  --wd /home/louis/bench-runs/dl-s0/run2 --out /home/louis/bench-runs/dl-s0/raw2.jsonl \
  --status /home/louis/bench-runs/dl-s0/run2.status --port 27800 --rounds 3 \
  --cells join8i,select8,select1,cabin8,pool8,tick8 > driver2.log 2>&1
date -u +%F\ %T > driver2.end
