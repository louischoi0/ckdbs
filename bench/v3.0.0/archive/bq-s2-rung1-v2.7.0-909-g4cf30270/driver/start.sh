#!/bin/sh
mkdir -p /home/cdkbs/bench-runs/bq/s2/out
nohup sh /home/cdkbs/bench-runs/bq/s2/watchdog.sh > /dev/null 2>&1 &
echo $! > /home/cdkbs/bench-runs/bq/s2/out/watchdog.pid
echo "watchdog pid $!"
