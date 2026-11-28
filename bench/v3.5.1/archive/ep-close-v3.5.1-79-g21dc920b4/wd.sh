#!/bin/bash
# wd.sh <plan-json>: every 60s relaunch the driver if its pid is gone and all.done absent
OUT=/home/ukjin/workspaces/ckdbs/.claude/worktrees/ep-bench/out
D=/tmp/claude-1000/-home-ukjin-workspaces-ckdbs/723f5d04-e5e6-4e9d-baf6-0a9577a8c72f/scratchpad/ep_ab.py
mkdir -p $OUT
alive(){ kill -0 "$1" 2>/dev/null && tr '\0' ' ' < /proc/$1/cmdline | grep -q ep_ab.py; }
launch(){ nohup python3 $D "$1" >> $OUT/driver.out 2>&1 & echo "$(date -u +%T) launched $!" >> $OUT/wd.log; }
[ -e $OUT/all.done ] || launch "$1"
while true; do sleep 60
  [ -e $OUT/all.done ] && { echo "$(date -u +%T) done" >> $OUT/wd.log; exit 0; }
  pid=$(cat $OUT/driver.pid 2>/dev/null || echo 0)
  age=$(( $(date +%s) - $(stat -c %Y $OUT/status 2>/dev/null || echo 0) ))
  if ! alive $pid; then echo "$(date -u +%T) driver gone, resuming" >> $OUT/wd.log; launch "$1"
  elif [ $age -gt 300 ]; then echo "$(date -u +%T) stalled ${age}s, restart" >> $OUT/wd.log; pkill -P $pid; kill $pid; sleep 2; launch "$1"
  else echo "$(date -u +%T) alive $pid age ${age}s" >> $OUT/wd.log; fi
done
