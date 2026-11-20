#!/bin/bash
RUN=/home/louis/bench-runs/scenarios-v3.5.0
echo $$ > "$RUN/watchdog3.pid"
while [ ! -e "$RUN/ab4.done" ]; do
  pid=$(cat "$RUN/ab4.pid" 2>/dev/null); age=$(( $(date +%s) - $(stat -c %Y "$RUN/status") ))
  echo "$(date -u +%FT%TZ) ab4=$pid status_age=${age}s status=$(cat $RUN/status)" >> "$RUN/watchdog3.log"
  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    echo "$(date -u +%FT%TZ) ORPHAN ab4: restarting" >> "$RUN/watchdog3.log"
    setsid nohup bash "$RUN/ab4.sh" >> "$RUN/logs/ab4.out" 2>&1 < /dev/null &
  fi
  sleep 60
done
