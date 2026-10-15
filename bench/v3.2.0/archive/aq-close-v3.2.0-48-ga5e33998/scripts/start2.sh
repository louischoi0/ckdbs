#!/bin/bash
cd /home/cdkbs/bench-runs/aq-close
cat /proc/loadavg; pgrep -c cc1plus
nohup ./scripts/run2.sh > out/run2.sh.out 2>&1 &
echo $! > run2.pid
nohup ./scripts/watch2.sh > /dev/null 2>&1 &
sleep 2; tail -n 3 progress.txt
