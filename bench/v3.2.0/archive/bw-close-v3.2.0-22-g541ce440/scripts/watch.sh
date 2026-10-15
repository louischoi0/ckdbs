#!/bin/bash
R=/home/cdkbs/bench-runs/bw-close
while [ ! -f $R/run.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch phase=${cur:-none} driver=$alive last: $(tail -n 1 $R/out/$cur.log 2>/dev/null | cut -c1-160)" >> $R/progress.txt
  if [ -f $R/run.pid ] && ! kill -0 "$(cat $R/run.pid)" 2>/dev/null && [ ! -f $R/run.done ]; then
    echo "$(date -u +%T) ORPHAN at $cur, resuming" >> $R/progress.txt
    (nohup $R/scripts/run.sh > $R/out/run.sh.out 2>&1 & echo $! > $R/run.pid)
  fi
  sleep 60
done
