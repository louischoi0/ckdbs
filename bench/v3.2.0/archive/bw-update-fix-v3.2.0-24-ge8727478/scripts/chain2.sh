#!/bin/bash
R=/home/cdkbs/bench-runs/bw-update-fix
while kill -0 $(cat $R/flags/u1.pid) 2>/dev/null; do sleep 3; done
[ -f $R/out/u1.json ] && { touch $R/flags/u1.done; echo "$(date -u +%T) phase end u1 (json present)" >> $R/progress.txt; }
$R/scripts/run2.sh
