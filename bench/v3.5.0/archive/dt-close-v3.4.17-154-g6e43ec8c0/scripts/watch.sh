#!/bin/bash
# exits (notifying the parent) when the run finishes, run.sh is gone, or the heartbeat is stale > 150 s
D=/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0
while true; do
  sleep 60
  if [ -e $D/finished4 ]; then echo "$(date -u +%T) finished"; exit 0; fi
  hb=$D/heartbeat; [ -e $D/finished ] && hb=$D/dr4.beat
  age=$(( $(date +%s) - $(stat -c %Y $hb 2>/dev/null || date +%s) ))
  echo "$(date -u +%T) hb age ${age}s: $(cat $hb 2>/dev/null)" >> /home/ukjin/bench-runs/dt/close/watch2.log
  if ! pgrep -f 'scripts/run3[.]sh' >/dev/null; then echo "$(date -u +%T) run.sh gone"; exit 1; fi
  if [ $age -gt 150 ]; then echo "$(date -u +%T) STALE $age"; exit 2; fi
done
