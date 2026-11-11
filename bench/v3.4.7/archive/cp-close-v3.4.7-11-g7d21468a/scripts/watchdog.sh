#!/bin/bash
# The 60 s watchdog over the CP-S0 schedule: one line a minute with the
# schedule's last phase and the heartbeat's age; exits when the schedule
# writes DONE. Polls files the schedule writes, never a process pattern.
S=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close/run
sleep 60
while true; do
  now=$(date +%s)
  hb=$(stat -c %Y -- "$S/heartbeat" 2>/dev/null)
  hb=${hb:-0}
  age=$(( now - hb ))
  last=$(tail -n 1 -- "$S/status.txt" 2>/dev/null)
  if [ "$age" -gt 90 ]; then
    echo "STALE heartbeat ${age}s: $last"
  else
    echo "ok hb ${age}s | $last | load $(cut -d' ' -f1 /proc/loadavg)"
  fi
  case "$last" in *DONE*) echo "schedule finished"; exit 0;; esac
  sleep 60
done
