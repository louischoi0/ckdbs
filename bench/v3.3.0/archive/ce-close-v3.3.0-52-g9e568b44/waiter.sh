#!/bin/bash
# waiter.sh FLAG [MAXSEC]: exits when $RUN/done/FLAG (or all.done for FLAG=all) exists, or after MAXSEC (default 540); prints the state.
R=/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44
F=${1:-all}
MAX=${2:-540}
n=0
while [ "$n" -lt "$MAX" ]; do
    [ "$F" = all ] && [ -e $R/all.done ] && break
    [ -e $R/done/$F ] && break
    sleep 5; n=$((n + 5))
done
echo "now: $(date -u +%FT%TZ) waited ${n}s"
tail -1 $R/watchdog.log | cut -c1-250
echo "status: $(cat $R/status)"
tail -6 $R/timeline
echo "load: $(cut -d' ' -f1-3 /proc/loadavg)  free KB: $(df --output=avail / | tail -1)"
