#!/bin/bash
# 60 s watchdog, polled by file: appends one line per minute to watch4.txt; exits at ALLDONE.
cd /home/louis/dfclose/run
while true; do
  last=$(ls -t log_r4_*.txt 2>/dev/null | head -1)
  age=$(( $(date +%s) - $(stat -c %Y ${last:-status4.txt}) ))
  echo "$(date -u +%FT%TZ) newest=$last age=${age}s load=$(cut -d' ' -f1 /proc/loadavg) last=$(tail -1 status4.txt)" >> watch4.txt
  grep -q "r4 ALLDONE" status4.txt 2>/dev/null && exit 0
  sleep 60
done
