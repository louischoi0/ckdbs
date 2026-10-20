#!/bin/bash
# watchdog.sh: every 60 s, if the run is not alive and no done flag exists, resume it (the run is resumable).
R=/home/cdkbs/bench-runs/row-cost
A=/home/cdkbs/ckdbs/.claude/worktrees/row-cost-attribution/bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c
launch() {
  nohup $A/run.sh run --kds-port 15690 --kds-pid $(cat $R/kds.pid) --pg-port 15434 --out $R/cells.jsonl \
        --progress $R/progress --reps 3 >> $R/run.log 2>&1 &
  echo $! > $R/run.pid
  echo "$(date -u +%T) launched $(cat $R/run.pid)" >> $R/watchdog.log
}
launch
while [ ! -e $R/done ]; do
  sleep 60
  if ! kill -0 $(cat $R/run.pid) 2>/dev/null; then
    if tail -c 300 $R/run.log | grep -q FINISHED; then touch $R/done; break; fi
    echo "$(date -u +%T) run gone, resuming" >> $R/watchdog.log
    launch
  fi
  echo "$(date -u +%T) $(cat $R/progress 2>/dev/null)" >> $R/watchdog.log
done
