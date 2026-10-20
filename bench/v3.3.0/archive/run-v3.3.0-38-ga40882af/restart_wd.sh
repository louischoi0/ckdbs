#!/bin/bash
R=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
setsid nohup bash $R/watchdog.sh >/dev/null 2>&1 < /dev/null &
sleep 3
tail -2 $R/watchdog.log
