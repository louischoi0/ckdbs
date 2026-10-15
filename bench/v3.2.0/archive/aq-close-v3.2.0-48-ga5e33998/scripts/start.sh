#!/bin/bash
cd /home/cdkbs/bench-runs/aq-close
cat /proc/loadavg; pgrep -c cc1plus
rm -f out/smoke.json; rm -rf wd-smoke
nohup ./scripts/run.sh > out/run.sh.out 2>&1 &
echo $! > run.pid
nohup ./scripts/watch.sh > /dev/null 2>&1 &
sleep 2; tail -n 3 progress.txt
