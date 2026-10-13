#!/bin/bash
# poll.sh SECONDS : wait up to SECONDS (or until run2.done), then print the run's state
R=/home/cdkbs/bench-runs/bt-close
t0=$(date +%s)
while [ ! -f $R/run2.done ] && [ $(( $(date +%s) - t0 )) -lt "$1" ]; do sleep 2; done
date -u +%T
cat /proc/loadavg
tail -4 $R/progress
cur=$(cat $R/flags/current 2>/dev/null)
echo "current=$cur"
tail -2 $R/watch.log
tail -3 $R/out/$cur.log | cut -c1-220
ls $R/flags/*.done 2>/dev/null
exit 0
