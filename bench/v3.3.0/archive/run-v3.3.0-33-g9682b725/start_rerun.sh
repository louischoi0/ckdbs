#!/bin/bash
R=/home/cdkbs/bench-runs/v3.3.0-33-g9682b725
sed -e 's#all.done#rerun.done#g' -e 's#monitor.pid#monitor3.pid#' $R/monitor.sh > $R/monitor3.sh
setsid nohup bash $R/monitor3.sh >/dev/null 2>&1 < /dev/null &
sleep 3
setsid nohup bash $R/run_rerun.sh >> $R/logs/master.out 2>&1 < /dev/null &
sleep 5
cat $R/status
