#!/bin/bash
# 60 s watchdog, polled by file: one line a minute to watchk.txt; exits at ALLDONE.
cd /home/louis/dfclose/run
while true; do
  last=$(ls -t log_k_*.txt 2>/dev/null | head -1)
  age=$(( $(date +%s) - $(stat -c %Y ${last:-statusk.txt}) ))
  echo "$(date -u +%FT%TZ) newest=$last age=${age}s load=$(cut -d' ' -f1 /proc/loadavg) last=$(tail -1 statusk.txt)" >> watchk.txt
  grep -q "k ALLDONE" statusk.txt 2>/dev/null && exit 0
  sleep 60
done
