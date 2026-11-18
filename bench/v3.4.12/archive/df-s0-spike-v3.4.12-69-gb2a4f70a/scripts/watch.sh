#!/bin/bash
# 60 s watchdog: logs progress-file tail and newest wd/out file age; polls the file, not a process pattern
R=/home/louis/bench-runs/df-s0
while ! grep -q DONE $R/progress.txt; do
  last=$(tail -1 $R/progress.txt)
  newest=$(find $R/wd-* $R/out -type f -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
  age=$(( $(date +%s) - ${newest%.*} ))
  echo "$(date -u '+%T') newest-file-age=${age}s load=$(cut -d' ' -f1 /proc/loadavg) cc1plus=$(pgrep -c cc1plus) last: $last" >> $R/watch.txt
  sleep 60
done
echo "$(date -u '+%T') DONE" >> $R/watch.txt
