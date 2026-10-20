#!/bin/bash
# wait.sh SECONDS : sleep, then print the run's state
R=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
sleep ${1:-60}
echo "now: $(date -u +%FT%TZ)"
tail -2 $R/watchdog.log | cut -c1-250
echo "status: $(cat $R/status)"
tail -4 $R/timeline
echo "cells: $(wc -l < $R/rowcost/cells.jsonl 2>/dev/null) valid, $(wc -l < $R/rowcost/cells.jsonl.contaminated 2>/dev/null) contaminated"
echo "cc1plus: $(pgrep cc1plus | wc -l)  load: $(cut -d' ' -f1-3 /proc/loadavg)"
