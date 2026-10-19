#!/bin/bash
# 60 s watchdog: polls the progress file the drivers write; restarts run.sh (resumable) if it died.
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
while [ ! -f $R/run.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch phase=${cur:-none} driver=$alive last: $(tail -n 1 $R/out/$cur.log 2>/dev/null | cut -c1-140)" >> $R/watch.txt
  if [ -f $R/run.pid ] && ! kill -0 "$(cat $R/run.pid)" 2>/dev/null && [ ! -f $R/run.done ]; then
    echo "$(date -u +%T) ORPHAN at $cur, resuming" >> $R/progress.txt
    (nohup $S/run.sh > $R/out/run.sh.out 2>&1 & echo $! > $R/run.pid)
  fi
  sleep 60
done
