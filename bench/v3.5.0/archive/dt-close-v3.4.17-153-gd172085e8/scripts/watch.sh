#!/bin/bash
# exits (notifying the parent) when the run finishes or its heartbeat is stale > 150 s
D=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-153-gd172085e8
while true; do
  sleep 60
  if [ -e $D/finished ]; then echo "$(date -u +%T) finished"; exit 0; fi
  age=$(( $(date +%s) - $(stat -c %Y $D/heartbeat) ))
  echo "$(date -u +%T) heartbeat age ${age}s: $(cat $D/heartbeat)" >> /home/ukjin/bench-runs/dt/close/watch.log
  if ! pgrep -f close/run.sh >/dev/null; then echo "$(date -u +%T) run.sh gone"; exit 1; fi
  if [ $age -gt 150 ]; then echo "$(date -u +%T) STALE age $age"; exit 2; fi
done
