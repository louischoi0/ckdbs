#!/bin/bash
# usage: watch.sh <donefile-marker-text> ; every 60 s writes status of the newest log; exits at ALLDONE
cd /home/louis/dfclose/run
while true; do
  last=$(ls -t log_*.txt 2>/dev/null | head -1)
  age=$(( $(date +%s) - $(stat -c %Y ${last:-status.txt}) ))
  alive=$(pgrep -f "bn_census.py" | wc -l)
  echo "$(date -u +%FT%TZ) newest=$last age=${age}s drivers=$alive load=$(cut -d' ' -f1 /proc/loadavg) last_status=$(tail -1 status.txt)" >> watch.txt
  grep -q "$1 ALLDONE" status.txt 2>/dev/null && exit 0
  sleep 60
done
