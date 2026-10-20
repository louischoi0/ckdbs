#!/bin/bash
R=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
mkdir -p $R/logs
date -u +%FT%TZ > $R/start-time.txt
setsid nohup bash $R/monitor.sh >/dev/null 2>&1 < /dev/null &
setsid nohup bash $R/watchdog.sh >/dev/null 2>&1 < /dev/null &
sleep 5
cat $R/status 2>/dev/null; tail -2 $R/monitor.log; tail -2 $R/watchdog.log
