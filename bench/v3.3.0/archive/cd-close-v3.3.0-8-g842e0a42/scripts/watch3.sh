#!/bin/bash
# 60 s watchdog for run3.sh: polls the progress file, restarts run3.sh (resumable) if it died.
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
while [ ! -f $R/run3.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch3 phase=${cur:-none} driver=$alive last: $(tail -n 1 $R/progress.txt | cut -c1-140)" >> $R/watch.txt
  if [ -f $R/run3.pid ] && ! kill -0 "$(cat $R/run3.pid)" 2>/dev/null && [ ! -f $R/run3.done ]; then
    echo "$(date -u +%T) ORPHAN3 at $cur, resuming" >> $R/progress.txt
    (nohup $S/run3.sh > $R/out/run3.sh.out 2>&1 & echo $! > $R/run3.pid)
  fi
  sleep 60
done
