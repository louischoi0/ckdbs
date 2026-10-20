#!/bin/bash
RC=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost
R=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
while [ ! -e $R/post.done ]; do sleep 10; done
sleep 10
setsid nohup bash $RC/watchdog_rc.sh >/dev/null 2>&1 < /dev/null &
