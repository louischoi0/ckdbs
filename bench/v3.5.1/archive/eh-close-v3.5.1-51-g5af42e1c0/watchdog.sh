#!/bin/bash
# 60 s watchdog over EF-S0: the master by its pid file, the status file by its age.
# The run is not idempotent per cell, so an orphan is reported and restarted whole at most once.
O=/home/ukjin/bench-runs/eh-close-v3.5.1-51-g5af42e1c0; tries=0
:
:
while [ ! -e $O/done/ALL ] && [ ! -e $O/failed ]; do
  mp=$(cat $O/master.pid 2>/dev/null); age=$(( $(date +%s) - $(stat -c %Y $O/status 2>/dev/null || date +%s) ))
  echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$(kill -0 $mp 2>/dev/null && echo y || echo n) status_age=${age}s $(cat $O/status 2>/dev/null)" >> $O/watchdog.log
  if { ! kill -0 "$mp" 2>/dev/null || [ $age -gt 900 ]; } && [ $tries -lt 1 ]; then
    echo "$(date -u +%FT%TZ) ORPHAN or STALL: restarting whole" >> $O/watchdog.log
    kill -TERM -- "-$mp" 2>/dev/null; pkill -f "$O/kds_server" 2>/dev/null; tries=1
     setsid nohup bash $O/run.sh >> $O/master.out 2>&1 < /dev/null &
  elif ! kill -0 "$mp" 2>/dev/null; then touch $O/failed; fi
  sleep 60
done
echo "$(date -u +%FT%TZ) end: $(ls $O/done/ALL $O/failed 2>/dev/null)" >> $O/watchdog.log
