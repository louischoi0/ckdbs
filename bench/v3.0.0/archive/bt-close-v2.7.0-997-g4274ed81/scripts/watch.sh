#!/bin/bash
# 60 s watchdog: writes one status line per minute to $R/watch.log; resumes run.sh if its driver was orphaned.
# Detects liveness by pid files and log mtimes, never by a process-name pattern.
R=/home/cdkbs/bench-runs/bt-close
echo $$ > $R/watch.pid
while [ ! -f $R/run.done ] && [ ! -f $R/watch.stop ]; do
  cur=$(cat $R/flags/current 2>/dev/null)
  runpid=$(cat $R/run.pid 2>/dev/null)
  drvpid=$(cat $R/flags/$cur.pid 2>/dev/null)
  runalive=no; drvalive=no
  [ -n "$runpid" ] && kill -0 "$runpid" 2>/dev/null && runalive=yes
  [ -n "$drvpid" ] && kill -0 "$drvpid" 2>/dev/null && drvalive=yes
  age=-1
  if [ -n "$cur" ] && [ -f $R/out/$cur.log ]; then
    age=$(( $(date +%s) - $(stat -c %Y $R/out/$cur.log) ))
  fi
  recs=$(grep -c . $R/out/$cur.log 2>/dev/null)
  echo "$(date -u +%T) phase=${cur:-none} run.sh=$runalive driver=$drvalive log_age=${age}s log_lines=${recs:-0} load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $R/watch.log
  if [ -n "$cur" ] && [ "$runalive" = no ] && [ ! -f $R/run.done ]; then
    # orphaned: run.sh gone before ALL DONE. Kill a leftover driver of the current phase by pid, then resume.
    [ "$drvalive" = yes ] && kill "$drvpid" 2>/dev/null
    echo "$(date -u +%T) ORPHANED at phase $cur - resuming run.sh" >> $R/watch.log
    echo "orphaned at $cur $(date -u +%T), resumed" >> $R/progress
    (nohup $R/scripts/run.sh > $R/out/run.sh.out 2>&1 &)
  fi
  sleep 60
done
echo "$(date -u +%T) watchdog exit" >> $R/watch.log
