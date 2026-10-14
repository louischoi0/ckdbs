#!/bin/bash
R=/home/cdkbs/bench-runs/bw-close
while [ ! -f $R/run.done ]; do sleep 3; done
$R/scripts/run2.sh > $R/out/run2.sh.out 2>&1 &
p=$!
while [ ! -f $R/run2.done ]; do
  cur=$(cat $R/flags/current 2>/dev/null); pid=$(cat $R/flags/$cur.pid 2>/dev/null)
  alive=no; [ -n "$pid" ] && kill -0 $pid 2>/dev/null && alive=yes
  echo "$(date -u +%T) watch2 phase=$cur driver=$alive last: $(tail -n 1 $R/out/$cur.log 2>/dev/null | cut -c1-160)" >> $R/progress.txt
  if ! kill -0 $p 2>/dev/null && [ ! -f $R/run2.done ]; then
    echo "$(date -u +%T) ORPHAN2 at $cur, resuming" >> $R/progress.txt
    $R/scripts/run2.sh >> $R/out/run2.sh.out 2>&1 &
    p=$!
  fi
  sleep 60
done
