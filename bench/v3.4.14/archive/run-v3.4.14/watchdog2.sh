#!/bin/bash
RUN=/home/louis/bench-runs/scenarios-v3.4.14
echo $$ > "$RUN/watchdog2.pid"
chk() { # name script
  local pid; pid=$(cat "$RUN/$1.pid" 2>/dev/null)
  if [ ! -e "$RUN/$1.done" ] && { [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; }; then
    echo "$(date -u +%FT%TZ) ORPHAN $1: restarting" >> "$RUN/watchdog2.log"
    setsid nohup bash "$RUN/$2" >> "$RUN/logs/$1.out" 2>&1 < /dev/null &
  fi
}
while [ ! -e "$RUN/sat2.done" ]; do
  now=$(date +%s); age=$(( now - $(stat -c %Y "$RUN/status") ))
  echo "$(date -u +%FT%TZ) ab=$(cat $RUN/ab.pid 2>/dev/null) extras=$(cat $RUN/extras.pid 2>/dev/null) status_age=${age}s status=$(cat $RUN/status)" >> "$RUN/watchdog2.log"
  chk ab ab.sh; chk extras extras.sh; chk pg pg.sh; chk sat2 sat2.sh
  sleep 60
done
