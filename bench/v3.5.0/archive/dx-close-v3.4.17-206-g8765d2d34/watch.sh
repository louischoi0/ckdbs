#!/bin/bash
B=/home/ukjin/bench-runs/dx-close
while kill -0 $(cat $B/driver.pid) 2>/dev/null; do
  now=$(date +%s); hb=$(python3 -c "import json;print(int(json.load(open('$B/run2/hb.json'))['t']))" 2>/dev/null || echo $now)
  echo "$(date -u +%T) age=$((now-hb)) $(cat /proc/loadavg)" >> $B/watch2.log
  sleep 60
done
echo "driver exited $(date -u +%T)" >> $B/watch2.log
