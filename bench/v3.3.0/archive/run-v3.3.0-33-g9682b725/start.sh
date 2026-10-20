#!/bin/bash
R=/home/cdkbs/bench-runs/v3.3.0-33-g9682b725
mkdir -p $R/logs
setsid nohup bash $R/monitor.sh >/dev/null 2>&1 < /dev/null &
setsid nohup bash $R/watchdog.sh >/dev/null 2>&1 < /dev/null &
sleep 25
cat $R/status; cat $R/timeline; ls $R/logs; tail -5 $R/logs/s1-c1-g.stdout
