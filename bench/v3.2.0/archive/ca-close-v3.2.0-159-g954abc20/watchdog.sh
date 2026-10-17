#!/bin/bash
# 60 s watchdog: logs status age; flags a stalled run (status unchanged > 6 min and no done file).
D=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-close-v3.2.0-159-g954abc20
while [ ! -e $D/runs/done ]; do
  A=$(( $(date +%s) - $(stat -c %Y $D/runs/status) ))
  echo "$(date -u +%T) status-age=${A}s $(cat $D/runs/status) load=$(cut -d' ' -f1 /proc/loadavg)" >> $D/watchdog.log
  [ $A -gt 360 ] && echo "$(date -u +%T) STALL" >> $D/watchdog.log
  sleep 60
done
echo "$(date -u +%T) done" >> $D/watchdog.log
