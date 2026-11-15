#!/bin/bash
R=/home/louis/bench-runs/f4/run
while ! grep -q "ALL DONE" $R/full.progress 2>/dev/null; do
  date -u +%T > $R/watchdog.txt; cat /proc/loadavg >> $R/hostload.log; pgrep -a cc1plus >> $R/hostload.log
  if ! pgrep -f "scripts/f4_ab.py" >/dev/null; then echo ORPHAN >> $R/watchdog.txt; break; fi
  sleep 60
done
