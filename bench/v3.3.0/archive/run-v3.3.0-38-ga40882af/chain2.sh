#!/bin/bash
R=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
while [ ! -e $R/plan.done ]; do sleep 10; done
sleep 8
setsid nohup bash $R/monitor3.sh >/dev/null 2>&1 < /dev/null &
setsid nohup bash $R/watchdog3.sh >/dev/null 2>&1 < /dev/null &
