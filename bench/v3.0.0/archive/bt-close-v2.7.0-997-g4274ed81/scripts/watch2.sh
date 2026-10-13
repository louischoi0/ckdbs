#!/bin/bash
# 60 s watchdog for run2.sh (same logic as watch.sh).  Starts watching once run.done exists.
R=/home/cdkbs/bench-runs/bt-close
echo $$ > $R/watch2.pid
while [ ! -f $R/run.done ]; do sleep 5; done
while [ ! -f $R/run2.done ] && [ ! -f $R/watch2.stop ]; do
  cur=$(cat $R/flags/current 2>/dev/null)
  runpid=$(cat $R/run2.pid 2>/dev/null)
  drvpid=$(cat $R/flags/$cur.pid 2>/dev/null)
  runalive=no; drvalive=no
  [ -n "$runpid" ] && kill -0 "$runpid" 2>/dev/null && runalive=yes
  [ -n "$drvpid" ] && kill -0 "$drvpid" 2>/dev/null && drvalive=yes
  age=-1
  if [ -n "$cur" ] && [ -f $R/out/$cur.log ]; then
    age=$(( $(date +%s) - $(stat -c %Y $R/out/$cur.log) ))
  fi
  recs=$(grep -c . $R/out/$cur.log 2>/dev/null)
  echo "$(date -u +%T) phase=${cur:-none} run2.sh=$runalive driver=$drvalive log_age=${age}s log_lines=${recs:-0} load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $R/watch.log
  if [ "$runalive" = no ] && [ ! -f $R/run2.done ]; then
    [ "$drvalive" = yes ] && kill "$drvpid" 2>/dev/null
    echo "$(date -u +%T) ORPHANED at phase $cur - resuming run2.sh" >> $R/watch.log
    echo "orphaned at $cur $(date -u +%T), resumed run2" >> $R/progress
    (nohup $R/scripts/run2.sh > $R/out/run2.sh.out 2>&1 &)
  fi
  sleep 60
done
echo "$(date -u +%T) watchdog2 exit" >> $R/watch.log
