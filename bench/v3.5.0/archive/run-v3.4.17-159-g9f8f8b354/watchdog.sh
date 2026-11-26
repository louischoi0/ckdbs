#!/bin/bash
# 60 s watchdog. Polls the status file's age, the current cell's stdout age and the
# runner's recorded PID - never a process pattern. An orphaned run (runner gone without
# DONE, or nothing written for 420 s) is resumed at once: run.sh skips every cell whose
# .rc is 0.
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
LOG=$R/run/watchdog.log
while :; do
  sleep 60
  if [ -f $R/run/DONE ]; then echo "$(date -u +%FT%TZ) DONE seen, watchdog exits" >> $LOG; exit 0; fi
  pid=$(cat $R/run/runner.pid 2>/dev/null)
  now=$(date +%s)
  cur=$(cat $R/run/current 2>/dev/null)
  m1=$(stat -c %Y $R/run/status 2>/dev/null || echo 0)
  m2=$(stat -c %Y "$cur" 2>/dev/null || echo 0)
  m=$m1; [ $m2 -gt $m ] && m=$m2
  age=$((now-m))
  alive=no
  if [ -n "$pid" ] && kill -0 $pid 2>/dev/null; then alive=yes; fi
  echo "$(date -u +%FT%TZ) runner=$pid alive=$alive age=${age}s last: $(tail -1 $R/run/status 2>/dev/null)" >> $LOG
  if [ $alive = no ] || [ $age -gt 420 ]; then
    echo "$(date -u +%FT%TZ) ORPHANED (alive=$alive age=${age}s): resuming" >> $LOG
    if [ $alive = yes ]; then pkill -TERM -P $pid; kill -TERM $pid; fi
    for p in $(cat $R/run/server.pids 2>/dev/null); do
      case "$(readlink /proc/$p/exe 2>/dev/null)" in $R/bin/*) kill -KILL $p ;; esac
    done
    : > $R/run/server.pids
    sleep 2
    setsid nohup bash $R/run.sh >> $R/run/runner.log 2>&1 < /dev/null &
    echo "$(date -u +%FT%TZ) resumed" >> $LOG
  fi
done
