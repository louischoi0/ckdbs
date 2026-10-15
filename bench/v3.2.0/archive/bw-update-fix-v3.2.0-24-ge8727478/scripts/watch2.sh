#!/bin/bash
R=/home/cdkbs/bench-runs/bw-update-fix
while [ ! -f $R/run.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch phase=${cur:-none} driver=$alive load=$(cut -d' ' -f1 /proc/loadavg) last: $(tail -n 1 $R/out/$cur.log 2>/dev/null | cut -c1-100)" >> $R/progress.txt
  if ! kill -0 "$(cat $R/run.pid)" 2>/dev/null && [ ! -f $R/run.done ]; then
    echo "$(date -u +%T) ORPHAN at $cur, resuming" >> $R/progress.txt
    (nohup $R/scripts/chain2.sh > $R/out/chain2.out 2>&1 & echo $! > $R/run.pid)
  fi
  sleep 60
done
