#!/bin/bash
# 60 s watchdog, polled by file: one line a minute to watchf.txt; exits at ALLDONE.
cd /home/louis/dfclose/run
while true; do
  last=$(ls -t log_f_*.txt 2>/dev/null | head -1)
  age=$(( $(date +%s) - $(stat -c %Y ${last:-statusf.txt}) ))
  echo "$(date -u +%FT%TZ) newest=$last age=${age}s load=$(cut -d' ' -f1 /proc/loadavg) last=$(tail -1 statusf.txt)" >> watchf.txt
  grep -q "f ALLDONE" statusf.txt 2>/dev/null && exit 0
  sleep 60
done
