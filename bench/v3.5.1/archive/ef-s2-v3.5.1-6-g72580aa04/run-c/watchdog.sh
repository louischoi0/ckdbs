#!/bin/bash
# 60 s watchdog over EF-S2 run c: an orphaned master (pid gone, done/ALL absent) is relaunched at once, a stalled one
# (status older than 1500 s: scenario 4's cell is about 300 s) is killed with its servers and relaunched.
# run.sh is idempotent per cell.
O=/home/ukjin/bench-runs/ef-v3.5.1-6-g72580aa04/s2c; R=/home/ukjin/bench-runs/ef-v3.5.1-6-g72580aa04
launch() { setsid nohup bash $O/run.sh >> $O/logs/master.out 2>&1 < /dev/null & }
while [ ! -e $O/done/ALL ] && [ ! -e $O/failed ]; do
  mp=$(cat $O/master.pid 2>/dev/null); age=$(( $(date +%s) - $(stat -c %Y $O/status 2>/dev/null || date +%s) ))
  echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$([ -n "$mp" ] && kill -0 $mp 2>/dev/null && echo y || echo n) status_age=${age}s $(cat $O/status 2>/dev/null)" >> $O/watchdog.log
  if [ -z "$mp" ] || ! kill -0 $mp 2>/dev/null; then echo "$(date -u +%FT%TZ) ORPHAN: launching" >> $O/watchdog.log; pkill -f "$R/kds_server" 2>/dev/null; launch
  elif [ $age -gt 1500 ]; then echo "$(date -u +%FT%TZ) STALL: relaunching" >> $O/watchdog.log; kill -TERM -- "-$mp" 2>/dev/null; sleep 2; kill -KILL -- "-$mp" 2>/dev/null; pkill -f "$R/kds_server" 2>/dev/null; launch; fi
  sleep 60
done
echo "$(date -u +%FT%TZ) end: $(ls $O/done/ALL $O/failed 2>/dev/null)" >> $O/watchdog.log
