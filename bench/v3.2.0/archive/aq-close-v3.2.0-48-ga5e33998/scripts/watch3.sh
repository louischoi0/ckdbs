#!/bin/bash
R=/home/cdkbs/bench-runs/aq-close
while [ ! -f $R/run3.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch3 phase=${cur:-none} driver=$alive last: $(tail -n 1 $R/out/$cur.log 2>/dev/null | cut -c1-140)" >> $R/progress.txt
  if [ -f $R/run3.pid ] && ! kill -0 "$(cat $R/run3.pid)" 2>/dev/null && [ ! -f $R/run3.done ]; then
    echo "$(date -u +%T) ORPHAN3 at $cur, resuming" >> $R/progress.txt
    (nohup $R/scripts/run3.sh > $R/out/run3.sh.out 2>&1 & echo $! > $R/run3.pid)
  fi
  sleep 60
done
