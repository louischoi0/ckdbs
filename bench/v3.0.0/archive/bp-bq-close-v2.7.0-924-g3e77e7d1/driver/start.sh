#!/bin/sh
mkdir -p /home/cdkbs/bench-runs/close/out
nohup sh /home/cdkbs/bench-runs/close/watchdog.sh > /dev/null 2>&1 &
echo $! > /home/cdkbs/bench-runs/close/out/watchdog.pid
echo "watchdog pid $!"
